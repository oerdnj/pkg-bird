/*
 *	BIRD -- OSPF Topological Database
 *
 *	(c) 1999       Martin Mares <mj@ucw.cz>
 *	(c) 1999--2004 Ondrej Filip <feela@network.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#include "nest/bird.h"
#include "lib/string.h"

#include "ospf.h"

#define HASH_DEF_ORDER 6
#define HASH_HI_MARK *4
#define HASH_HI_STEP 2
#define HASH_HI_MAX 16
#define HASH_LO_MARK /5
#define HASH_LO_STEP 2
#define HASH_LO_MIN 8

void originate_prefix_rt_lsa(struct ospf_area *oa);
void originate_prefix_net_lsa(struct ospf_iface *ifa);
void flush_prefix_net_lsa(struct ospf_iface *ifa);

#ifdef OSPFv2
#define ipa_to_rid(x) _I(x)
#else /* OSPFv3 */
#define ipa_to_rid(x) _I3(x)
#endif


#ifdef OSPFv2
static inline u32
fibnode_to_lsaid(struct proto_ospf *po, struct fib_node *fn)
{
  /* We have to map IP prefixes to u32 in such manner that resulting
     u32 interpreted as IP address is a member of given
     prefix. Therefore, /32 prefix have to be mapped on itself.
     All received prefixes have to be mapped on different u32s.

     We have an assumption that if there is nontrivial (non-/32)
     network prefix, then there is not /32 prefix for the first
     and the last IP address of the network (these are usually
     reserved, therefore it is not an important restriction).
     The network prefix is mapped to the first or the last
     IP address in the manner that disallow collisions - we
     use IP address that cannot be used by parent prefix.

     For example:
     192.168.0.0/24 maps to 192.168.0.255
     192.168.1.0/24 maps to 192.168.1.0
     because 192.168.0.0 and 192.168.1.255 might be used by
     192.168.0.0/23 .

     This is not compatible with older RFC 1583, so we have an option
     to the RFC 1583 compatible assignment (that always uses the first
     address) which disallows subnetting.

     Appendig E of RFC 2328 suggests different algorithm, that tries
     to maximize both compatibility and subnetting. But as it is not
     possible to have both reliably and the suggested algorithm was
     unnecessary complicated and it does crazy things like changing
     LSA ID for a network because different network appeared, we
     choose a different way. */

  u32 id = _I(fn->prefix);

  if ((po->rfc1583) || (fn->pxlen == 0) || (fn->pxlen == 32))
    return id;

  if (id & (1 << (32 - fn->pxlen)))
    return id;
  else
    return id | ~u32_mkmask(fn->pxlen);
}

#else /* OSPFv3 */

static inline u32
fibnode_to_lsaid(struct proto_ospf *po, struct fib_node *fn)
{
  /* In OSPFv3, it is simpler. There is not a requirement
     for membership of the result in input network, so we
     just use hash-based unique ID. */

  return fn->uid;
}

#endif


static void *
lsab_alloc(struct proto_ospf *po, unsigned size)
{
  unsigned offset = po->lsab_used;
  po->lsab_used += size;
  if (po->lsab_used > po->lsab_size)
    {
      po->lsab_size = MAX(po->lsab_used, 2 * po->lsab_size);
      po->lsab = mb_realloc(po->proto.pool, po->lsab, po->lsab_size);
    }
  return ((byte *) po->lsab) + offset;
}

static inline void *
lsab_allocz(struct proto_ospf *po, unsigned size)
{
  void *r = lsab_alloc(po, size);
  bzero(r, size);
  return r;
}

static inline void *
lsab_flush(struct proto_ospf *po)
{
  void *r = mb_alloc(po->proto.pool, po->lsab_size);
  memcpy(r, po->lsab, po->lsab_used);
  po->lsab_used = 0;
  return r;
}

static inline void *
lsab_offset(struct proto_ospf *po, unsigned offset)
{
  return ((byte *) po->lsab) + offset;
}

static inline void *
lsab_end(struct proto_ospf *po)
{
  return ((byte *) po->lsab) + po->lsab_used;
}


static int
configured_stubnet(struct ospf_area *oa, struct ifa *a)
{
  struct ospf_stubnet_config *sn;
  WALK_LIST(sn, oa->ac->stubnet_list)
    {
      if (sn->summary)
	{
	  if (ipa_in_net(a->prefix, sn->px.addr, sn->px.len) && (a->pxlen >= sn->px.len))
	    return 1;
	}
      else
	{
	  if (ipa_equal(a->prefix, sn->px.addr) && (a->pxlen == sn->px.len))
	    return 1;
	}
    }
  return 0;
}

int
bcast_net_active(struct ospf_iface *ifa)
{
  struct ospf_neighbor *neigh;

  if (ifa->state == OSPF_IS_WAITING)
    return 0;

  WALK_LIST(neigh, ifa->neigh_list)
    {
      if (neigh->state == NEIGHBOR_FULL)
	{
	  if (neigh->rid == ifa->drid)
	    return 1;

	  if (ifa->state == OSPF_IS_DR)
	    return 1;
	}
    }

  return 0;
}


#ifdef OSPFv2

static void *
originate_rt_lsa_body(struct ospf_area *oa, u16 *length)
{
  struct proto_ospf *po = oa->po;
  struct ospf_iface *ifa;
  int i = 0, bitv = 0;
  struct ospf_lsa_rt *rt;
  struct ospf_lsa_rt_link *ln;
  struct ospf_neighbor *neigh;

  ASSERT(po->lsab_used == 0);
  rt = lsab_allocz(po, sizeof(struct ospf_lsa_rt));

  rt->options = 0;

  if (po->areano > 1)
    rt->options |= OPT_RT_B;

  if ((po->ebit) && (!oa->stub))
    rt->options |= OPT_RT_E;

  rt = NULL; /* buffer might be reallocated later */

  WALK_LIST(ifa, po->iface_list)
  {
    int net_lsa = 0;

    if ((ifa->type == OSPF_IT_VLINK) && (ifa->voa == oa) &&
	(!EMPTY_LIST(ifa->neigh_list)))
    {
      neigh = (struct ospf_neighbor *) HEAD(ifa->neigh_list);
      if ((neigh->state == NEIGHBOR_FULL) && (ifa->cost <= 0xffff))
	bitv = 1;
    }

    if ((ifa->oa != oa) || (ifa->state == OSPF_IS_DOWN))
      continue;

    /* BIRD does not support interface loops */
    ASSERT(ifa->state != OSPF_IS_LOOP);

    switch (ifa->type)
      {
      case OSPF_IT_PTP:	/* RFC2328 - 12.4.1.1 */
	neigh = (struct ospf_neighbor *) HEAD(ifa->neigh_list);
	if ((!EMPTY_LIST(ifa->neigh_list)) && (neigh->state == NEIGHBOR_FULL))
	{
	  ln = lsab_alloc(po, sizeof(struct ospf_lsa_rt_link));
	  ln->type = LSART_PTP;
	  ln->id = neigh->rid;
	  ln->data = (ifa->addr->flags & IA_UNNUMBERED) ?
	    ifa->iface->index : ipa_to_u32(ifa->addr->ip);
	  ln->metric = ifa->cost;
	  ln->padding = 0;
	  i++;
	}
	break;

      case OSPF_IT_BCAST: /* RFC2328 - 12.4.1.2 */
      case OSPF_IT_NBMA:
	if (bcast_net_active(ifa))
	  {
	    ln = lsab_alloc(po, sizeof(struct ospf_lsa_rt_link));
	    ln->type = LSART_NET;
	    ln->id = ipa_to_u32(ifa->drip);
	    ln->data = ipa_to_u32(ifa->addr->ip);
	    ln->metric = ifa->cost;
	    ln->padding = 0;
	    i++;
	    net_lsa = 1;
	  }
	break;

      case OSPF_IT_VLINK: /* RFC2328 - 12.4.1.3 */
	neigh = (struct ospf_neighbor *) HEAD(ifa->neigh_list);
	if ((!EMPTY_LIST(ifa->neigh_list)) && (neigh->state == NEIGHBOR_FULL) && (ifa->cost <= 0xffff))
	{
	  ln = lsab_alloc(po, sizeof(struct ospf_lsa_rt_link));
	  ln->type = LSART_VLNK;
	  ln->id = neigh->rid;
	  ln->data = ipa_to_u32(ifa->addr->ip);
	  ln->metric = ifa->cost;
	  ln->padding = 0;
	  i++;
        }
        break;

      default:
        log("Unknown interface type %s", ifa->iface->name);
        break;
      }

    /* Now we will originate stub area if there is no primary */
    if (net_lsa ||
	(ifa->type == OSPF_IT_VLINK) ||
	(ifa->addr->flags & IA_UNNUMBERED) ||
	configured_stubnet(oa, ifa->addr))
      continue;

    ln = lsab_alloc(po, sizeof(struct ospf_lsa_rt_link));
    ln->type = LSART_STUB;
    ln->id = ipa_to_u32(ifa->addr->prefix);
    ln->data = ipa_to_u32(ipa_mkmask(ifa->addr->pxlen));
    ln->metric = ifa->cost;
    ln->padding = 0;
    i++;
  }

  struct ospf_stubnet_config *sn;
  WALK_LIST(sn, oa->ac->stubnet_list)
    if (!sn->hidden)
      {
	ln = lsab_alloc(po, sizeof(struct ospf_lsa_rt_link));
	ln->type = LSART_STUB;
	ln->id = ipa_to_u32(sn->px.addr);
	ln->data = ipa_to_u32(ipa_mkmask(sn->px.len));
	ln->metric = sn->cost;
	ln->padding = 0;
	i++;
      }

  rt = po->lsab;
  rt->links = i;

  if (bitv) 
    rt->options |= OPT_RT_V;

  *length = po->lsab_used + sizeof(struct ospf_lsa_header);
  return lsab_flush(po);
}

#else /* OSPFv3 */

static void
add_lsa_rt_link(struct proto_ospf *po, struct ospf_iface *ifa, u8 type, u32 nif, u32 id)
{
  struct ospf_lsa_rt_link *ln = lsab_alloc(po, sizeof(struct ospf_lsa_rt_link));
  ln->type = type;
  ln->padding = 0;
  ln->metric = ifa->cost;
  ln->lif = ifa->iface->index;
  ln->nif = nif;
  ln->id = id;
}

static void *
originate_rt_lsa_body(struct ospf_area *oa, u16 *length)
{
  struct proto_ospf *po = oa->po;
  struct ospf_iface *ifa;
  int bitv = 0;
  struct ospf_lsa_rt *rt;
  struct ospf_neighbor *neigh;

  ASSERT(po->lsab_used == 0);
  rt = lsab_allocz(po, sizeof(struct ospf_lsa_rt));

  rt->options = oa->options & OPTIONS_MASK;

  if (po->areano > 1)
    rt->options |= OPT_RT_B;

  if ((po->ebit) && (!oa->stub))
    rt->options |= OPT_RT_E;

  rt = NULL; /* buffer might be reallocated later */

  WALK_LIST(ifa, po->iface_list)
  {
    if ((ifa->type == OSPF_IT_VLINK) && (ifa->voa == oa) &&
	(!EMPTY_LIST(ifa->neigh_list)))
    {
      neigh = (struct ospf_neighbor *) HEAD(ifa->neigh_list);
      if ((neigh->state == NEIGHBOR_FULL) && (ifa->cost <= 0xffff))
	bitv = 1;
    }

    if ((ifa->oa != oa) || (ifa->state == OSPF_IS_DOWN))
      continue;

    /* BIRD does not support interface loops */
    ASSERT(ifa->state != OSPF_IS_LOOP);

    /* RFC5340 - 4.4.3.2 */
    switch (ifa->type)
      {
      case OSPF_IT_PTP:
	neigh = (struct ospf_neighbor *) HEAD(ifa->neigh_list);
	if ((!EMPTY_LIST(ifa->neigh_list)) && (neigh->state == NEIGHBOR_FULL))
	  add_lsa_rt_link(po, ifa, LSART_PTP, neigh->iface_id, neigh->rid);
	break;

      case OSPF_IT_BCAST:
      case OSPF_IT_NBMA:
	if (bcast_net_active(ifa))
	  add_lsa_rt_link(po, ifa, LSART_NET, ifa->dr_iface_id, ifa->drid);
	break;

      case OSPF_IT_VLINK:
	neigh = (struct ospf_neighbor *) HEAD(ifa->neigh_list);
	if ((!EMPTY_LIST(ifa->neigh_list)) && (neigh->state == NEIGHBOR_FULL) && (ifa->cost <= 0xffff))
	  add_lsa_rt_link(po, ifa, LSART_VLNK, neigh->iface_id, neigh->rid);
        break;

      default:
        log("Unknown interface type %s", ifa->iface->name);
        break;
      }
  }

  if (bitv)
    {
      rt = po->lsab;
      rt->options |= OPT_RT_V;
    }

  *length = po->lsab_used + sizeof(struct ospf_lsa_header);
  return lsab_flush(po);
}

#endif

/**
 * originate_rt_lsa - build new instance of router LSA
 * @oa: ospf_area which is LSA built to
 *
 * It builds router LSA walking through all OSPF interfaces in
 * specified OSPF area. This function is mostly called from
 * area_disp(). Builds new LSA, increases sequence number (if old
 * instance exists) and sets age of LSA to zero.
 */
void
originate_rt_lsa(struct ospf_area *oa)
{
  struct ospf_lsa_header lsa;
  struct proto_ospf *po = oa->po;
  struct proto *p = &po->proto;
  void *body;

  OSPF_TRACE(D_EVENTS, "Originating router-LSA for area %R", oa->areaid);

  lsa.age = 0;
  lsa.type = LSA_T_RT;
  
#ifdef OSPFv2
  lsa.options = oa->options;
  lsa.id = po->router_id;
#else /* OSPFv3 */
  lsa.id = 0;
#endif

  lsa.rt = po->router_id;
  lsa.sn = oa->rt ? (oa->rt->lsa.sn + 1) : LSA_INITSEQNO;
  u32 dom = oa->areaid;

  body = originate_rt_lsa_body(oa, &lsa.length);
  lsasum_calculate(&lsa, body);
  oa->rt = lsa_install_new(po, &lsa, dom, body);
  ospf_lsupd_flood(po, NULL, NULL, &lsa, dom, 1);
}

void
update_rt_lsa(struct ospf_area *oa)
{
  struct proto_ospf *po = oa->po;

  if ((oa->rt) && ((oa->rt->inst_t + MINLSINTERVAL)) > now)
    return;
  /*
   * Tick is probably set to very low value. We cannot
   * originate new LSA before MINLSINTERVAL. We will
   * try to do it next tick.
   */

  originate_rt_lsa(oa);
#ifdef OSPFv3
  originate_prefix_rt_lsa(oa);
#endif

  schedule_rtcalc(po);
  oa->origrt = 0;
}

static void *
originate_net_lsa_body(struct ospf_iface *ifa, u16 *length,
		       struct proto_ospf *po)
{
  u16 i = 1;
  struct ospf_neighbor *n;
  struct ospf_lsa_net *net;
  int nodes = ifa->fadj + 1;

  net = mb_alloc(po->proto.pool, sizeof(struct ospf_lsa_net)
		 + nodes * sizeof(u32));

#ifdef OSPFv2
  net->netmask = ipa_mkmask(ifa->addr->pxlen);
#endif

#ifdef OSPFv3
  /* In OSPFv3, we would like to merge options from Link LSAs of added neighbors */
  struct top_hash_entry *en;
  u32 options = 0;
#endif

  net->routers[0] = po->router_id;

  WALK_LIST(n, ifa->neigh_list)
  {
    if (n->state == NEIGHBOR_FULL)
    {
#ifdef OSPFv3
      en = ospf_hash_find(po->gr, ifa->iface->index, n->iface_id, n->rid, LSA_T_LINK);
      if (en)
	options |= ((struct ospf_lsa_link *) en->lsa_body)->options;
#endif

      net->routers[i] = n->rid;
      i++;
    }
  }
  ASSERT(i == nodes);

#ifdef OSPFv3
  net->options = options & OPTIONS_MASK;
#endif
  
  *length = sizeof(struct ospf_lsa_header) + sizeof(struct ospf_lsa_net)
    + nodes * sizeof(u32);
  return net;
}


/**
 * originate_net_lsa - originates of deletes network LSA
 * @ifa: interface which is LSA originated for
 *
 * Interface counts number of adjacent neighbors. If this number is
 * lower than one or interface is not in state %OSPF_IS_DR it deletes
 * and premature ages instance of network LSA for specified interface.
 * In other case, new instance of network LSA is originated.
 */
void
originate_net_lsa(struct ospf_iface *ifa)
{
  struct proto_ospf *po = ifa->oa->po;
  struct proto *p = &po->proto;
  struct ospf_lsa_header lsa;
  u32 dom = ifa->oa->areaid;
  
  void *body;

  OSPF_TRACE(D_EVENTS, "Originating network-LSA for iface %s",
	     ifa->iface->name);

  lsa.age = 0;
  lsa.type = LSA_T_NET;

#ifdef OSPFv2
  lsa.options = ifa->oa->options;
  lsa.id = ipa_to_u32(ifa->addr->ip);
#else /* OSPFv3 */
  lsa.id = ifa->iface->index;
#endif

  lsa.rt = po->router_id;
  lsa.sn = ifa->net_lsa ? (ifa->net_lsa->lsa.sn + 1) : LSA_INITSEQNO;

  body = originate_net_lsa_body(ifa, &lsa.length, po);
  lsasum_calculate(&lsa, body);
  ifa->net_lsa = lsa_install_new(po, &lsa, dom, body);
  ospf_lsupd_flood(po, NULL, NULL, &lsa, dom, 1);
}

void
flush_net_lsa(struct ospf_iface *ifa)
{
  struct proto_ospf *po = ifa->oa->po;
  struct proto *p = &po->proto;
  u32 dom = ifa->oa->areaid;

  if (ifa->net_lsa == NULL)
    return;

  OSPF_TRACE(D_EVENTS, "Flushing network-LSA for iface %s",
	     ifa->iface->name);
  ifa->net_lsa->lsa.sn += 1;
  ifa->net_lsa->lsa.age = LSA_MAXAGE;
  lsasum_calculate(&ifa->net_lsa->lsa, ifa->net_lsa->lsa_body);
  ospf_lsupd_flood(po, NULL, NULL, &ifa->net_lsa->lsa, dom, 0);
  flush_lsa(ifa->net_lsa, po);
  ifa->net_lsa = NULL;
}

void
update_net_lsa(struct ospf_iface *ifa)
{
  struct proto_ospf *po = ifa->oa->po;
 
  if (ifa->net_lsa && ((ifa->net_lsa->inst_t + MINLSINTERVAL) > now))
    return;
  /*
   * It's too early to originate new network LSA. We will
   * try to do it next tick
   */

  if ((ifa->state != OSPF_IS_DR) || (ifa->fadj == 0))
    {
      flush_net_lsa(ifa);
#ifdef OSPFv3
      flush_prefix_net_lsa(ifa);
#endif
    }
  else
    {
      originate_net_lsa(ifa);
#ifdef OSPFv3
      originate_prefix_net_lsa(ifa);
#endif
    }

  schedule_rtcalc(po);
  ifa->orignet = 0;
}

#ifdef OSPFv2

static inline void *
originate_sum_lsa_body(struct proto_ospf *po, u16 *length, u32 mlen, u32 metric)
{
  struct ospf_lsa_sum *sum = mb_alloc(po->proto.pool, sizeof(struct ospf_lsa_sum));
  *length = sizeof(struct ospf_lsa_header) + sizeof(struct ospf_lsa_sum);

  sum->netmask = ipa_mkmask(mlen);
  sum->metric = metric;

  return sum;
}

#define originate_sum_net_lsa_body(po,length,fn,metric) \
  originate_sum_lsa_body(po, length, (fn)->pxlen, metric)

#define originate_sum_rt_lsa_body(po,length,drid,metric,options) \
  originate_sum_lsa_body(po, length, 0, metric)

static inline int
check_sum_net_lsaid_collision(struct fib_node *fn, struct top_hash_entry *en)
{
  struct ospf_lsa_sum *sum = en->lsa_body;
  return fn->pxlen != ipa_mklen(sum->netmask);
}

static inline int
check_ext_lsaid_collision(struct fib_node *fn, struct top_hash_entry *en)
{
  struct ospf_lsa_ext *ext = en->lsa_body;
  return fn->pxlen != ipa_mklen(ext->netmask);
}

#else /* OSPFv3 */

static inline void *
originate_sum_net_lsa_body(struct proto_ospf *po, u16 *length, struct fib_node *fn, u32 metric)
{
  int size = sizeof(struct ospf_lsa_sum_net) + IPV6_PREFIX_SPACE(fn->pxlen);
  struct ospf_lsa_sum_net *sum = mb_alloc(po->proto.pool, size);
  *length = sizeof(struct ospf_lsa_header) + size;

  sum->metric = metric;
  put_ipv6_prefix(sum->prefix, fn->prefix, fn->pxlen, 0, 0);

  return sum;
}

static inline int
check_sum_net_lsaid_collision(struct fib_node *fn, struct top_hash_entry *en)
{
  struct ospf_lsa_sum_net *sum = en->lsa_body;
  ip_addr prefix;
  int pxlen;
  u8 pxopts;
  u16 rest;

  lsa_get_ipv6_prefix(sum->prefix, &prefix, &pxlen, &pxopts, &rest);
  return (fn->pxlen != pxlen) || !ipa_equal(fn->prefix, prefix);
}

static inline int
check_ext_lsaid_collision(struct fib_node *fn, struct top_hash_entry *en)
{
  struct ospf_lsa_ext *ext = en->lsa_body;
  ip_addr prefix;
  int pxlen;
  u8 pxopts;
  u16 rest;

  lsa_get_ipv6_prefix(ext->rest, &prefix, &pxlen, &pxopts, &rest);
  return (fn->pxlen != pxlen) || !ipa_equal(fn->prefix, prefix);
}

static inline void *
originate_sum_rt_lsa_body(struct proto_ospf *po, u16 *length, u32 drid, u32 metric, u32 options)
{
  struct ospf_lsa_sum_rt *sum = mb_alloc(po->proto.pool, sizeof(struct ospf_lsa_sum_rt));
  *length = sizeof(struct ospf_lsa_header) + sizeof(struct ospf_lsa_sum_rt);

  sum->options = options & OPTIONS_MASK;
  sum->metric = metric;
  sum->drid = drid;

  return sum;
}

#endif

void
originate_sum_net_lsa(struct ospf_area *oa, struct fib_node *fn, int metric)
{
  struct proto_ospf *po = oa->po;
  struct proto *p = &po->proto;
  struct top_hash_entry *en;
  u32 dom = oa->areaid;
  struct ospf_lsa_header lsa;
  void *body;

  OSPF_TRACE(D_EVENTS, "Originating net-summary-LSA for %I/%d (metric %d)",
	     fn->prefix, fn->pxlen, metric);

  /* options argument is used in ORT_NET and OSPFv3 only */
  lsa.age = 0;
#ifdef OSPFv2
  lsa.options = oa->options;
#endif
  lsa.type = LSA_T_SUM_NET;
  lsa.id = fibnode_to_lsaid(po, fn);
  lsa.rt = po->router_id;
  lsa.sn = LSA_INITSEQNO;

  if ((en = ospf_hash_find_header(po->gr, dom, &lsa)) != NULL)
  {
    if (check_sum_net_lsaid_collision(fn, en))
      {
	log(L_ERR, "%s: LSAID collision for %I/%d",
	    p->name, fn->prefix, fn->pxlen);
	return;
      }

    lsa.sn = en->lsa.sn + 1;
  }

  body = originate_sum_net_lsa_body(po, &lsa.length, fn, metric);
  lsasum_calculate(&lsa, body);
  en = lsa_install_new(po, &lsa, dom, body);
  ospf_lsupd_flood(po, NULL, NULL, &lsa, dom, 1);
}

void
originate_sum_rt_lsa(struct ospf_area *oa, struct fib_node *fn, int metric, u32 options UNUSED)
{
  struct proto_ospf *po = oa->po;
  struct proto *p = &po->proto;
  struct top_hash_entry *en;
  u32 dom = oa->areaid;
  u32 rid = ipa_to_rid(fn->prefix);
  struct ospf_lsa_header lsa;
  void *body;

  OSPF_TRACE(D_EVENTS, "Originating rt-summary-LSA for %R (metric %d)",
	     rid, metric);

  lsa.age = 0;
#ifdef OSPFv2
  lsa.options = oa->options;
#endif
  lsa.type = LSA_T_SUM_RT;
  /* In OSPFv3, LSA ID is meaningless, but we still use Router ID of ASBR */
  lsa.id = rid;
  lsa.rt = po->router_id;
  lsa.sn = LSA_INITSEQNO;

  if ((en = ospf_hash_find_header(po->gr, dom, &lsa)) != NULL)
    lsa.sn = en->lsa.sn + 1;

  body = originate_sum_rt_lsa_body(po, &lsa.length, lsa.id, metric, options);
  lsasum_calculate(&lsa, body);
  en = lsa_install_new(po, &lsa, dom, body);
  ospf_lsupd_flood(po, NULL, NULL, &lsa, dom, 1);
}


void
originate_sum_lsa(struct ospf_area *oa, struct fib_node *fn, int type, int metric, u32 options)
{
  if (type == ORT_NET)
    originate_sum_net_lsa(oa, fn, metric);
  else
    originate_sum_rt_lsa(oa, fn, metric, options);
}


void
flush_sum_lsa(struct ospf_area *oa, struct fib_node *fn, int type)
{
  struct proto_ospf *po = oa->po;
  struct proto *p = &po->proto;
  struct top_hash_entry *en;
  struct ospf_lsa_header lsa;

  lsa.rt = po->router_id;
  if (type == ORT_NET)
    {
      lsa.id = fibnode_to_lsaid(po, fn);
      lsa.type = LSA_T_SUM_NET;
    }
  else
    {
      /* In OSPFv3, LSA ID is meaningless, but we still use Router ID of ASBR */
      lsa.id = ipa_to_rid(fn->prefix);
      lsa.type = LSA_T_SUM_RT;
    }

  if ((en = ospf_hash_find_header(po->gr, oa->areaid, &lsa)) != NULL)
    {
      if ((type == ORT_NET) && check_sum_net_lsaid_collision(fn, en))
	{
	  log(L_ERR, "%s: LSAID collision for %I/%d",
	      p->name, fn->prefix, fn->pxlen);
	  return;
	}

      struct ospf_lsa_sum *sum = en->lsa_body;
      en->lsa.age = LSA_MAXAGE;
      en->lsa.sn = LSA_MAXSEQNO;
      lsasum_calculate(&en->lsa, sum);

      OSPF_TRACE(D_EVENTS, "Flushing summary-LSA (id=%R, type=%d)",
		 en->lsa.id, en->lsa.type);
      ospf_lsupd_flood(po, NULL, NULL, &en->lsa, oa->areaid, 1);
      if (can_flush_lsa(po)) flush_lsa(en, po);
    }
}


void
check_sum_lsa(struct proto_ospf *po, ort *nf, int dest)
{
  struct ospf_area *oa;
  int flush, mlen;
  ip_addr ip;

  if (po->areano < 2) return;

  if ((nf->n.type > RTS_OSPF_IA) && (nf->o.type > RTS_OSPF_IA)) return;

#ifdef LOCAL_DEBUG
  DBG("Checking...dest = %d, %I/%d\n", dest, nf->fn.prefix, nf->fn.pxlen);
  if (nf->n.oa) DBG("New: met=%d, oa=%d\n", nf->n.metric1, nf->n.oa->areaid);
  if (nf->o.oa) DBG("Old: met=%d, oa=%d\n", nf->o.metric1, nf->o.oa->areaid);
#endif

  WALK_LIST(oa, po->area_list)
  {
    flush = 0;
    if ((nf->n.metric1 >= LSINFINITY) || (nf->n.type > RTS_OSPF_IA))
      flush = 1;
    if ((dest == ORT_ROUTER) && (!(nf->n.options & ORTA_ASBR)))
      flush = 1;
    if ((!nf->n.oa) || (nf->n.oa->areaid == oa->areaid))
      flush = 1;

    if (nf->n.ifa) {
      if (nf->n.ifa->oa->areaid == oa->areaid)
        flush = 1;
      }
    else flush = 1;

    /* Don't send summary into stub areas
     * We send just default route (and later) */
    if (oa->stub)
      flush = 1;
    
    mlen = nf->fn.pxlen;
    ip = ipa_and(nf->fn.prefix, ipa_mkmask(mlen));

    if ((oa == po->backbone) && (nf->n.type == RTS_OSPF_IA))
      flush = 1;	/* Only intra-area can go to the backbone */

    if ((!flush) && (dest == ORT_NET) && fib_route(&nf->n.oa->net_fib, ip, mlen))
    {
      /* The route fits into area networks */
      flush = 1;
      if ((nf->n.oa == po->backbone) && (oa->trcap)) flush = 0;
    }

    if (flush)
      flush_sum_lsa(oa, &nf->fn, dest);
    else
      originate_sum_lsa(oa, &nf->fn, dest, nf->n.metric1, nf->n.options);
  }
}


static void *
originate_ext_lsa_body(net *n, rte *e, u16 *length, struct proto_ospf *po,
		       struct ea_list *attrs)
{
  struct proto *p = &po->proto;
  struct ospf_lsa_ext *ext;
  u32 m1 = ea_get_int(attrs, EA_OSPF_METRIC1, LSINFINITY);
  u32 m2 = ea_get_int(attrs, EA_OSPF_METRIC2, 10000);
  u32 tag = ea_get_int(attrs, EA_OSPF_TAG, 0);
  int gw = 0;
  int size = sizeof(struct ospf_lsa_ext);

  // FIXME check for gw should be per ifa, not per iface
  if ((e->attrs->dest == RTD_ROUTER) &&
      !ipa_equal(e->attrs->gw, IPA_NONE) &&
      !ipa_has_link_scope(e->attrs->gw) &&
      (ospf_iface_find((struct proto_ospf *) p, e->attrs->iface) != NULL))
    gw = 1;

#ifdef OSPFv3
  size += IPV6_PREFIX_SPACE(n->n.pxlen);

  if (gw)
    size += 16;
  
  if (tag)
    size += 4;
#endif
  
  ext = mb_alloc(p->pool, size);
  *length = sizeof(struct ospf_lsa_header) + size;

  ext->metric = (m1 != LSINFINITY) ? m1 : (m2 | LSA_EXT_EBIT); 

#ifdef OSPFv2
  ext->netmask = ipa_mkmask(n->n.pxlen);
  ext->fwaddr = gw ? e->attrs->gw : IPA_NONE;
  ext->tag = tag;
#else /* OSPFv3 */
  u32 *buf = ext->rest;
  buf = put_ipv6_prefix(buf, n->n.prefix, n->n.pxlen, 0, 0);

  if (gw)
    {
      ext->metric |= LSA_EXT_FBIT;
      buf = put_ipv6_addr(buf, e->attrs->gw);
    }

  if (tag)
    {
      ext->metric |= LSA_EXT_TBIT;
      *buf++ = tag;
    }
#endif

  return ext;
}

/**
 * originate_ext_lsa - new route received from nest and filters
 * @n: network prefix and mask
 * @e: rte
 * @po: current instance of OSPF
 * @attrs: list of extended attributes
 *
 * If I receive a message that new route is installed, I try to originate an
 * external LSA. The LSA header of such LSA does not contain information about
 * prefix length, so if I have to originate multiple LSAs for route with
 * different prefixes I try to increment prefix id to find a "free" one.
 *
 * The function also sets flag ebit. If it's the first time, the new router lsa
 * origination is necessary.
 */
void
originate_ext_lsa(net * n, rte * e, struct proto_ospf *po,
		  struct ea_list *attrs)
{
  struct proto *p = &po->proto;
  struct fib_node *fn = &n->n;
  struct ospf_lsa_header lsa;
  struct top_hash_entry *en = NULL;
  void *body;
  struct ospf_area *oa;

  OSPF_TRACE(D_EVENTS, "Originating AS-external-LSA for %I/%d",
	     fn->prefix, fn->pxlen);

  lsa.age = 0;
#ifdef OSPFv2
  lsa.options = 0; /* or oa->options ? */
#endif
  lsa.type = LSA_T_EXT;
  lsa.id = fibnode_to_lsaid(po, fn);
  lsa.rt = po->router_id;
  lsa.sn = LSA_INITSEQNO;

  if ((en = ospf_hash_find_header(po->gr, 0, &lsa)) != NULL)
    {
      if (check_ext_lsaid_collision(fn, en))
	{
	  log(L_ERR, "%s: LSAID collision for %I/%d",
	      p->name, fn->prefix, fn->pxlen);
	  return;
	}

      lsa.sn = en->lsa.sn + 1;
    }

  body = originate_ext_lsa_body(n, e, &lsa.length, po, attrs);
  lsasum_calculate(&lsa, body);

  en = lsa_install_new(po, &lsa, 0, body);
  ospf_lsupd_flood(po, NULL, NULL, &lsa, 0, 1);

  if (po->ebit == 0)
  {
    po->ebit = 1;
    WALK_LIST(oa, po->area_list)
    {
      schedule_rt_lsa(oa);
    }
  }
}

void
flush_ext_lsa(net *n, struct proto_ospf *po)
{
  struct proto *p = &po->proto;
  struct fib_node *fn = &n->n;
  struct top_hash_entry *en;

  OSPF_TRACE(D_EVENTS, "Flushing AS-external-LSA for %I/%d",
	     fn->prefix, fn->pxlen);

  u32 lsaid = fibnode_to_lsaid(po, fn);

  if (en = ospf_hash_find(po->gr, 0, lsaid, po->router_id, LSA_T_EXT))
    {
      if (check_ext_lsaid_collision(fn, en))
	{
	  log(L_ERR, "%s: LSAID collision for %I/%d",
	      p->name, fn->prefix, fn->pxlen);
	  return;
	}

      ospf_lsupd_flush_nlsa(po, en);
    }
}


#ifdef OSPFv3

static void *
originate_link_lsa_body(struct ospf_iface *ifa, u16 *length)
{
  struct proto_ospf *po = ifa->oa->po;
  struct ospf_lsa_link *ll;
  int i = 0;
  u8 flags;

  ASSERT(po->lsab_used == 0);
  ll = lsab_allocz(po, sizeof(struct ospf_lsa_link));
  ll->options = ifa->oa->options | (ifa->priority << 24);
  ll->lladdr = ifa->addr->ip;
  ll = NULL; /* buffer might be reallocated later */

  struct ifa *a;
  WALK_LIST(a, ifa->iface->addrs)
    {
      if ((a->flags & IA_SECONDARY) ||
	  (a->scope < SCOPE_SITE))
	continue;

      flags = (a->pxlen < MAX_PREFIX_LENGTH) ? 0 : OPT_PX_LA;
      put_ipv6_prefix(lsab_alloc(po, IPV6_PREFIX_SPACE(a->pxlen)),
		      a->ip, a->pxlen, flags, 0);
      i++;
    }

  ll = po->lsab;
  ll->pxcount = i;
  *length = po->lsab_used + sizeof(struct ospf_lsa_header);
  return lsab_flush(po);
}

void
originate_link_lsa(struct ospf_iface *ifa)
{
  struct ospf_lsa_header lsa;
  struct proto_ospf *po = ifa->oa->po;
  struct proto *p = &po->proto;
  void *body;

  /* FIXME check for vlink and skip that? */
  OSPF_TRACE(D_EVENTS, "Originating link-LSA for iface %s", ifa->iface->name);

  lsa.age = 0;
  lsa.type = LSA_T_LINK;
  lsa.id = ifa->iface->index;
  lsa.rt = po->router_id;
  lsa.sn = ifa->link_lsa ? (ifa->link_lsa->lsa.sn + 1) : LSA_INITSEQNO;
  u32 dom = ifa->iface->index;

  body = originate_link_lsa_body(ifa, &lsa.length);
  lsasum_calculate(&lsa, body);
  ifa->link_lsa = lsa_install_new(po, &lsa, dom, body);
  ospf_lsupd_flood(po, NULL, NULL, &lsa, dom, 1);

  /* Just to be sure to not forget on our link LSA */
  if (ifa->state == OSPF_IS_DR)
    schedule_net_lsa(ifa);
}

void
update_link_lsa(struct ospf_iface *ifa)
{
  if (ifa->link_lsa && ((ifa->link_lsa->inst_t + MINLSINTERVAL) > now))
    return;
  /*
   * It's too early to originate new link LSA. We will
   * try to do it next tick
   */
  originate_link_lsa(ifa);
  ifa->origlink = 0;
}

static void *
originate_prefix_rt_lsa_body(struct ospf_area *oa, u16 *length)
{
  struct proto_ospf *po = oa->po;
  struct ospf_iface *ifa;
  struct ospf_lsa_prefix *lp;
  int net_lsa;
  int i = 0;
  u8 flags;

  ASSERT(po->lsab_used == 0);
  lp = lsab_allocz(po, sizeof(struct ospf_lsa_prefix));
  lp->ref_type = LSA_T_RT;
  lp->ref_id = 0;
  lp->ref_rt = po->router_id;
  lp = NULL; /* buffer might be reallocated later */

  WALK_LIST(ifa, po->iface_list)
  {
    if ((ifa->oa != oa) || (ifa->state == OSPF_IS_DOWN))
      continue;

    if ((ifa->type == OSPF_IT_BCAST) ||
	(ifa->type == OSPF_IT_NBMA))
      net_lsa = bcast_net_active(ifa);
    else
      net_lsa = 0;

    struct ifa *a;
    WALK_LIST(a, ifa->iface->addrs)
      {
	if (((a->pxlen < MAX_PREFIX_LENGTH) && net_lsa) ||
	    (a->flags & IA_SECONDARY) ||
	    (a->flags & IA_UNNUMBERED) ||
	    (a->scope <= SCOPE_LINK) ||
	    configured_stubnet(oa, a))
	  continue;

	flags = (a->pxlen < MAX_PREFIX_LENGTH) ? 0 : OPT_PX_LA;
	put_ipv6_prefix(lsab_alloc(po, IPV6_PREFIX_SPACE(a->pxlen)),
			a->ip, a->pxlen, flags, ifa->cost);
	i++;
      }
  }

  /* FIXME Handle vlinks? see RFC5340, page 38 */

  struct ospf_stubnet_config *sn;
  WALK_LIST(sn, oa->ac->stubnet_list)
    if (!sn->hidden)
      {
	flags = (sn->px.len < MAX_PREFIX_LENGTH) ? 0 : OPT_PX_LA;
	put_ipv6_prefix(lsab_alloc(po, IPV6_PREFIX_SPACE(sn->px.len)),
			sn->px.addr, sn->px.len, flags, sn->cost);
	i++;
      }

  lp = po->lsab;
  lp->pxcount = i;
  *length = po->lsab_used + sizeof(struct ospf_lsa_header);
  return lsab_flush(po);
}

void
originate_prefix_rt_lsa(struct ospf_area *oa)
{
  struct proto_ospf *po = oa->po;
  struct proto *p = &po->proto;  
  struct ospf_lsa_header lsa;
  void *body;

  OSPF_TRACE(D_EVENTS, "Originating router prefix-LSA for area %R", oa->areaid);

  lsa.age = 0;
  lsa.type = LSA_T_PREFIX;
  lsa.id = 0;
  lsa.rt = po->router_id;
  lsa.sn = oa->pxr_lsa ? (oa->pxr_lsa->lsa.sn + 1) : LSA_INITSEQNO;
  u32 dom = oa->areaid;

  body = originate_prefix_rt_lsa_body(oa, &lsa.length);
  lsasum_calculate(&lsa, body);
  oa->pxr_lsa = lsa_install_new(po, &lsa, dom, body);
  ospf_lsupd_flood(po, NULL, NULL, &lsa, dom, 1);
}


static inline int
prefix_space(u32 *buf)
{
  int pxl = *buf >> 24;
  return IPV6_PREFIX_SPACE(pxl);
}

static inline int
prefix_same(u32 *b1, u32 *b2)
{
  int pxl1 = *b1 >> 24;
  int pxl2 = *b2 >> 24;
  int pxs, i;
  
  if (pxl1 != pxl2)
    return 0;

  pxs = IPV6_PREFIX_WORDS(pxl1);
  for (i = 1; i < pxs; i++)
    if (b1[i] != b2[i])
      return 0;

  return 1;
}

static inline u32 *
prefix_advance(u32 *buf)
{
  int pxl = *buf >> 24;
  return buf + IPV6_PREFIX_WORDS(pxl);
}

static void
add_prefix(struct proto_ospf *po, u32 *px, int offset, int *pxc)
{
  u32 *pxl = lsab_offset(po, offset);
  int i;
  for (i = 0; i < *pxc; i++)
    {
      if (prefix_same(px, pxl))
	{
	  /* Options should be logically OR'ed together */
	  *pxl |= *px;
	  return;
	}
      pxl = prefix_advance(pxl);
    }

  ASSERT(pxl == lsab_end(po));

  int pxspace = prefix_space(px);
  pxl = lsab_alloc(po, pxspace);
  memcpy(pxl, px, pxspace);
  (*pxc)++;
}

static void
add_link_lsa(struct proto_ospf *po, struct top_hash_entry *en, int offset, int *pxc)
{
  struct ospf_lsa_link *ll = en->lsa_body;
  u32 *pxb = ll->rest;
  int j;

  for (j = 0; j < ll->pxcount; j++)
    {
      add_prefix(po, pxb, offset, pxc);
      pxb = prefix_advance(pxb);
    }
}



static void *
originate_prefix_net_lsa_body(struct ospf_iface *ifa, u16 *length)
{
  struct proto_ospf *po = ifa->oa->po;
  struct ospf_lsa_prefix *lp;
  struct ospf_neighbor *n;
  struct top_hash_entry *en;
  int pxc, offset;

  ASSERT(po->lsab_used == 0);
  lp = lsab_allocz(po, sizeof(struct ospf_lsa_prefix));
  lp->ref_type = LSA_T_NET;
  lp->ref_id = ifa->net_lsa->lsa.id;
  lp->ref_rt = po->router_id;
  lp = NULL; /* buffer might be reallocated later */

  pxc = 0;
  offset = po->lsab_used;

  /* Find all Link LSAs associated with the link and merge their prefixes */
  if (ifa->link_lsa)
    add_link_lsa(po, ifa->link_lsa, offset, &pxc);

  WALK_LIST(n, ifa->neigh_list)
    if ((n->state == NEIGHBOR_FULL) &&
      	(en = ospf_hash_find(po->gr, ifa->iface->index, n->iface_id, n->rid, LSA_T_LINK)))
      add_link_lsa(po, en, offset, &pxc);

  lp = po->lsab;
  lp->pxcount = pxc;
  *length = po->lsab_used + sizeof(struct ospf_lsa_header);
  return lsab_flush(po);
}

void
originate_prefix_net_lsa(struct ospf_iface *ifa)
{
  struct proto_ospf *po = ifa->oa->po;
  struct proto *p = &po->proto;
  struct ospf_lsa_header lsa;
  void *body;

  OSPF_TRACE(D_EVENTS, "Originating network prefix-LSA for iface %s",
	     ifa->iface->name);

  lsa.age = 0;
  lsa.type = LSA_T_PREFIX;
  lsa.id = ifa->iface->index;
  lsa.rt = po->router_id;
  lsa.sn = ifa->pxn_lsa ? (ifa->pxn_lsa->lsa.sn + 1) : LSA_INITSEQNO;
  u32 dom = ifa->oa->areaid;

  body = originate_prefix_net_lsa_body(ifa, &lsa.length);
  lsasum_calculate(&lsa, body);
  ifa->pxn_lsa = lsa_install_new(po, &lsa, dom, body);
  ospf_lsupd_flood(po, NULL, NULL, &lsa, dom, 1);
}

void
flush_prefix_net_lsa(struct ospf_iface *ifa)
{
  struct proto_ospf *po = ifa->oa->po;
  struct proto *p = &po->proto;
  struct top_hash_entry *en = ifa->pxn_lsa;
  u32 dom = ifa->oa->areaid;

  if (en == NULL)
    return;

  OSPF_TRACE(D_EVENTS, "Flushing network prefix-LSA for iface %s",
	     ifa->iface->name);

  en->lsa.sn += 1;
  en->lsa.age = LSA_MAXAGE;
  lsasum_calculate(&en->lsa, en->lsa_body);
  ospf_lsupd_flood(po, NULL, NULL, &en->lsa, dom, 0);
  flush_lsa(en, po);
  ifa->pxn_lsa = NULL;
}


#endif


static void
ospf_top_ht_alloc(struct top_graph *f)
{
  f->hash_size = 1 << f->hash_order;
  f->hash_mask = f->hash_size - 1;
  if (f->hash_order > HASH_HI_MAX - HASH_HI_STEP)
    f->hash_entries_max = ~0;
  else
    f->hash_entries_max = f->hash_size HASH_HI_MARK;
  if (f->hash_order < HASH_LO_MIN + HASH_LO_STEP)
    f->hash_entries_min = 0;
  else
    f->hash_entries_min = f->hash_size HASH_LO_MARK;
  DBG("Allocating OSPF hash of order %d: %d hash_entries, %d low, %d high\n",
      f->hash_order, f->hash_size, f->hash_entries_min, f->hash_entries_max);
  f->hash_table =
    mb_alloc(f->pool, f->hash_size * sizeof(struct top_hash_entry *));
  bzero(f->hash_table, f->hash_size * sizeof(struct top_hash_entry *));
}

static inline void
ospf_top_ht_free(struct top_hash_entry **h)
{
  mb_free(h);
}

static inline u32
ospf_top_hash_u32(u32 a)
{
  /* Shamelessly stolen from IP address hashing in ipv4.h */
  a ^= a >> 16;
  a ^= a << 10;
  return a;
}

static inline unsigned
ospf_top_hash(struct top_graph *f, u32 domain, u32 lsaid, u32 rtrid, u32 type)
{
  /* In OSPFv2, we don't know Router ID when looking for network LSAs.
     In OSPFv3, we don't know LSA ID when looking for router LSAs.
     In both cases, there is (usually) just one (or small number)
     appropriate LSA, so we just clear unknown part of key. */

  return (
#ifdef OSPFv2
	  ((type == LSA_T_NET) ? 0 : ospf_top_hash_u32(rtrid)) +
	  ospf_top_hash_u32(lsaid) + 
#else /* OSPFv3 */
	  ospf_top_hash_u32(rtrid) +
	  ((type == LSA_T_RT) ? 0 : ospf_top_hash_u32(lsaid)) +
#endif
	  type + domain) & f->hash_mask;

  /*
  return (ospf_top_hash_u32(lsaid) + ospf_top_hash_u32(rtrid) +
	  type + areaid) & f->hash_mask;
  */
}

/**
 * ospf_top_new - allocated new topology database
 * @p: current instance of ospf
 *
 * this dynamically hashed structure is often used for keeping lsas. mainly
 * its used in @ospf_area structure.
 */
struct top_graph *
ospf_top_new(pool *pool)
{
  struct top_graph *f;

  f = mb_allocz(pool, sizeof(struct top_graph));
  f->pool = pool;
  f->hash_slab = sl_new(f->pool, sizeof(struct top_hash_entry));
  f->hash_order = HASH_DEF_ORDER;
  ospf_top_ht_alloc(f);
  f->hash_entries = 0;
  f->hash_entries_min = 0;
  return f;
}

void
ospf_top_free(struct top_graph *f)
{
  rfree(f->hash_slab);
  ospf_top_ht_free(f->hash_table);
  mb_free(f);
}

static void
ospf_top_rehash(struct top_graph *f, int step)
{
  unsigned int oldn, oldh;
  struct top_hash_entry **n, **oldt, **newt, *e, *x;

  oldn = f->hash_size;
  oldt = f->hash_table;
  DBG("re-hashing topology hash from order %d to %d\n", f->hash_order,
      f->hash_order + step);
  f->hash_order += step;
  ospf_top_ht_alloc(f);
  newt = f->hash_table;

  for (oldh = 0; oldh < oldn; oldh++)
  {
    e = oldt[oldh];
    while (e)
    {
      x = e->next;
      n = newt + ospf_top_hash(f, e->domain, e->lsa.id, e->lsa.rt, e->lsa.type);
      e->next = *n;
      *n = e;
      e = x;
    }
  }
  ospf_top_ht_free(oldt);
}

#ifdef OSPFv2

u32
ospf_lsa_domain(u32 type, struct ospf_iface *ifa)
{
  return (type == LSA_T_EXT) ? 0 : ifa->oa->areaid;
}

#else /* OSPFv3 */

u32
ospf_lsa_domain(u32 type, struct ospf_iface *ifa)
{
  switch (type & LSA_SCOPE_MASK)
    {
    case LSA_SCOPE_LINK:
      return ifa->iface->index;

    case LSA_SCOPE_AREA:
      return ifa->oa->areaid;

    case LSA_SCOPE_AS:
    default:
      return 0;
    }
}

#endif

struct top_hash_entry *
ospf_hash_find_header(struct top_graph *f, u32 domain, struct ospf_lsa_header *h)
{
  return ospf_hash_find(f, domain, h->id, h->rt, h->type);
}

struct top_hash_entry *
ospf_hash_get_header(struct top_graph *f, u32 domain, struct ospf_lsa_header *h)
{
  return ospf_hash_get(f, domain, h->id, h->rt, h->type);
}

struct top_hash_entry *
ospf_hash_find(struct top_graph *f, u32 domain, u32 lsa, u32 rtr, u32 type)
{
  struct top_hash_entry *e;
  e = f->hash_table[ospf_top_hash(f, domain, lsa, rtr, type)];

  while (e && (e->lsa.id != lsa || e->lsa.type != type || e->lsa.rt != rtr || e->domain != domain))
    e = e->next;

  return e;
}


#ifdef OSPFv2

/* In OSPFv2, sometimes we don't know Router ID when looking for network LSAs.
   There should be just one, so we find any match. */
struct top_hash_entry *
ospf_hash_find_net(struct top_graph *f, u32 domain, u32 lsa)
{
  struct top_hash_entry *e;
  e = f->hash_table[ospf_top_hash(f, domain, lsa, 0, LSA_T_NET)];

  while (e && (e->lsa.id != lsa || e->lsa.type != LSA_T_NET || e->domain != domain))
    e = e->next;

  return e;
}

#endif


#ifdef OSPFv3

/* In OSPFv3, usually we don't know LSA ID when looking for router
   LSAs. We return matching LSA with smallest LSA ID. */
struct top_hash_entry *
ospf_hash_find_rt(struct top_graph *f, u32 domain, u32 rtr)
{
  struct top_hash_entry *rv = NULL;
  struct top_hash_entry *e;
  e = f->hash_table[ospf_top_hash(f, domain, 0, rtr, LSA_T_RT)];
  
  while (e)
    {
      if (e->lsa.rt == rtr && e->lsa.type == LSA_T_RT && e->domain == domain)
	if (!rv || e->lsa.id < rv->lsa.id)
	  rv = e;
      e = e->next;
    }

  return rv;
}

static inline struct top_hash_entry *
find_matching_rt(struct top_hash_entry *e, u32 domain, u32 rtr)
{
  while (e && (e->lsa.rt != rtr || e->lsa.type != LSA_T_RT || e->domain != domain))
    e = e->next;
  return e;
}

struct top_hash_entry *
ospf_hash_find_rt_first(struct top_graph *f, u32 domain, u32 rtr)
{
  struct top_hash_entry *e;
  e = f->hash_table[ospf_top_hash(f, domain, 0, rtr, LSA_T_RT)];
  return find_matching_rt(e, domain, rtr);
}

struct top_hash_entry *
ospf_hash_find_rt_next(struct top_hash_entry *e)
{
  return find_matching_rt(e->next, e->domain, e->lsa.rt);
}

#endif


struct top_hash_entry *
ospf_hash_get(struct top_graph *f, u32 domain, u32 lsa, u32 rtr, u32 type)
{
  struct top_hash_entry **ee;
  struct top_hash_entry *e;

  ee = f->hash_table + ospf_top_hash(f, domain, lsa, rtr, type);
  e = *ee;

  while (e && (e->lsa.id != lsa || e->lsa.rt != rtr || e->lsa.type != type || e->domain != domain))
    e = e->next;

  if (e)
    return e;

  e = sl_alloc(f->hash_slab);
  e->color = OUTSPF;
  e->dist = LSINFINITY;
  e->nhi = NULL;
  e->nh = IPA_NONE;
  e->lb = IPA_NONE;
  e->lsa.id = lsa;
  e->lsa.rt = rtr;
  e->lsa.type = type;
  e->lsa_body = NULL;
  e->nhi = NULL;
  e->domain = domain;
  e->next = *ee;
  *ee = e;
  if (f->hash_entries++ > f->hash_entries_max)
    ospf_top_rehash(f, HASH_HI_STEP);
  return e;
}

void
ospf_hash_delete(struct top_graph *f, struct top_hash_entry *e)
{
  struct top_hash_entry **ee = f->hash_table + 
    ospf_top_hash(f, e->domain, e->lsa.id, e->lsa.rt, e->lsa.type);

  while (*ee)
  {
    if (*ee == e)
    {
      *ee = e->next;
      sl_free(f->hash_slab, e);
      if (f->hash_entries-- < f->hash_entries_min)
	ospf_top_rehash(f, -HASH_LO_STEP);
      return;
    }
    ee = &((*ee)->next);
  }
  bug("ospf_hash_delete() called for invalid node");
}

/*
static void
ospf_dump_lsa(struct top_hash_entry *he, struct proto *p)
{

  struct ospf_lsa_rt *rt = NULL;
  struct ospf_lsa_rt_link *rr = NULL;
  struct ospf_lsa_net *ln = NULL;
  u32 *rts = NULL;
  u32 i, max;

  OSPF_TRACE(D_EVENTS, "- %1x %-1R %-1R %4u 0x%08x 0x%04x %-1R",
	     he->lsa.type, he->lsa.id, he->lsa.rt, he->lsa.age, he->lsa.sn,
	     he->lsa.checksum, he->domain);


  switch (he->lsa.type)
    {
    case LSA_T_RT:
      rt = he->lsa_body;
      rr = (struct ospf_lsa_rt_link *) (rt + 1);

      for (i = 0; i < lsa_rt_items(&he->lsa); i++)
        OSPF_TRACE(D_EVENTS, "  - %1x %-1R %-1R %5u",
		   rr[i].type, rr[i].id, rr[i].data, rr[i].metric);
      break;

    case LSA_T_NET:
      ln = he->lsa_body;
      rts = (u32 *) (ln + 1);

      for (i = 0; i < lsa_net_items(&he->lsa); i++)
        OSPF_TRACE(D_EVENTS, "  - %-1R", rts[i]);
      break;

    default:
      break;
    }
}

void
ospf_top_dump(struct top_graph *f, struct proto *p)
{
  unsigned int i;
  OSPF_TRACE(D_EVENTS, "Hash entries: %d", f->hash_entries);

  for (i = 0; i < f->hash_size; i++)
  {
    struct top_hash_entry *e;
    for (e = f->hash_table[i]; e != NULL; e = e->next)
      ospf_dump_lsa(e, p);
  }
}
*/

/* This is very inefficient, please don't call it often */

/* I should also test for every LSA if it's in some link state
 * retransmission list for every neighbor. I will not test it.
 * It could happen that I'll receive some strange ls ack's.
 */

int
can_flush_lsa(struct proto_ospf *po)
{
  struct ospf_iface *ifa;
  struct ospf_neighbor *n;

  WALK_LIST(ifa, po->iface_list)
  {
    WALK_LIST(n, ifa->neigh_list)
    {
      if ((n->state == NEIGHBOR_EXCHANGE) || (n->state == NEIGHBOR_LOADING))
        return 0;

      break;
    }
  }

  return 1;
}

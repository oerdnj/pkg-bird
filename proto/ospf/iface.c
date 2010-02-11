/*
 *	BIRD -- OSPF
 *
 *	(c) 1999--2005 Ondrej Filip <feela@network.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#include "ospf.h"

char *ospf_is[] = { "down", "loop", "waiting", "point-to-point", "drother",
  "backup", "dr"
};

char *ospf_ism[] = { "interface up", "wait timer fired", "backup seen",
  "neighbor change", "loop indicated", "unloop indicated", "interface down"
};

char *ospf_it[] = { "broadcast", "nbma", "point-to-point", "virtual link" };

static void
poll_timer_hook(timer * timer)
{
  log("POLL!");
  ospf_hello_send(timer, 1, NULL);
}

static void
hello_timer_hook(timer * timer)
{
  ospf_hello_send(timer, 0, NULL);
}

static void
wait_timer_hook(timer * timer)
{
  struct ospf_iface *ifa = (struct ospf_iface *) timer->data;
  struct proto *p = &ifa->oa->po->proto;

  OSPF_TRACE(D_EVENTS, "Wait timer fired on interface %s.", ifa->iface->name);
  ospf_iface_sm(ifa, ISM_WAITF);
}

u32
rxbufsize(struct ospf_iface *ifa)
{
  switch(ifa->rxbuf)
  {
    case OSPF_RXBUF_NORMAL:
      return (ifa->iface->mtu * 2);
      break;
    case OSPF_RXBUF_LARGE:
      return OSPF_MAX_PKT_SIZE;
      break;
    default:
      return ifa->rxbuf;
      break;
  }
}

static sock *
ospf_open_socket(struct ospf_iface *ifa, int mc)
{
  sock *ipsk;
  struct proto *p = &ifa->oa->po->proto;

  ipsk = sk_new(p->pool);
  ipsk->type = SK_IP;
  ipsk->dport = OSPF_PROTO;

#ifdef OSPFv2
  /*
   * In Linux IPv4, binding a raw socket to an IP address of an iface causes
   * that the socket does not receive multicast packets, as they have
   * different (multicast) destination IP address.
   *
   * We want such filter in the vlink (non-mc) socket.
   */
  ipsk->saddr = mc ? IPA_NONE : ifa->iface->addr->ip;
#else /* OSPFv3 */
  ipsk->saddr = ifa->lladdr;
#endif

  ipsk->tos = IP_PREC_INTERNET_CONTROL;
  ipsk->ttl = 1;
  if (ifa->type == OSPF_IT_VLINK)
    ipsk->ttl = 255;
  ipsk->rx_hook = ospf_rx_hook;
  ipsk->tx_hook = ospf_tx_hook;
  ipsk->err_hook = ospf_err_hook;
  ipsk->iface = ifa->iface;
  ipsk->rbsize = rxbufsize(ifa);
  ipsk->tbsize = ifa->iface->mtu;
  ipsk->data = (void *) ifa;
  if (sk_open(ipsk) != 0)
    goto err;

#ifdef OSPFv3
  /* 12 is an offset of the checksum in an OSPF packet */
  if (sk_set_ipv6_checksum(ipsk, 12) < 0)
    goto err;
#endif

  if (mc)
  {
    if (sk_setup_multicast(ipsk) < 0)
      goto err;

    if (sk_join_group(ipsk, AllSPFRouters) < 0)
      goto err;
  }

  return ipsk;

 err:
  rfree(ipsk);
  return NULL;
}


/**
 * ospf_iface_chstate - handle changes of interface state
 * @ifa: OSPF interface
 * @state: new state
 *
 * Many actions must be taken according to interface state changes. New network
 * LSAs must be originated, flushed, new multicast sockets to listen for messages for
 * %ALLDROUTERS have to be opened, etc.
 */
void
ospf_iface_chstate(struct ospf_iface *ifa, u8 state)
{
  struct proto_ospf *po = ifa->oa->po;
  struct proto *p = &po->proto;
  u8 oldstate = ifa->state;

  if (oldstate != state)
  {
    ifa->state = state;

    if (ifa->type == OSPF_IT_VLINK)
    {
      OSPF_TRACE(D_EVENTS,
		 "Changing state of virtual link %R from \"%s\" into \"%s\".",
		 ifa->vid, ospf_is[oldstate], ospf_is[state]);
      if (state == OSPF_IS_PTP)
      {
        ifa->sk = ospf_open_socket(ifa, 0);
      }
    }
    else
    {
      OSPF_TRACE(D_EVENTS,
		 "Changing state of iface: %s from \"%s\" into \"%s\".",
		 ifa->iface->name, ospf_is[oldstate], ospf_is[state]);
      if (ifa->iface->flags & IF_MULTICAST)
      {
	if ((ifa->type != OSPF_IT_NBMA) && (ifa->ioprob == OSPF_I_OK) &&
	    ((state == OSPF_IS_BACKUP) || (state == OSPF_IS_DR)))
	{
	  if (!ifa->dr_up == 0)
	  {
	    /* FIXME some error handing ? */
	    sk_join_group(ifa->sk, AllDRouters);
	    ifa->dr_up = 1;
	  }
	}
	else if (ifa->dr_up)
	{
	  sk_leave_group(ifa->sk, AllDRouters);
	  ifa->dr_up = 0;
	}
	if ((oldstate == OSPF_IS_DR) && (ifa->net_lsa != NULL))
	{
	  ifa->net_lsa->lsa.age = LSA_MAXAGE;
	  if (state >= OSPF_IS_WAITING)
	  {
	    ospf_lsupd_flush_nlsa(po, ifa->net_lsa);
	  }
	  if (can_flush_lsa(po))
	    flush_lsa(ifa->net_lsa, po);
	  ifa->net_lsa = NULL;
	}
      }
    }
  }
}

static void
ospf_iface_down(struct ospf_iface *ifa)
{
  struct ospf_neighbor *n, *nx;
  struct proto_ospf *po = ifa->oa->po;
  struct proto *p = &po->proto;
  struct ospf_iface *iff;

  /* First of all kill all the related vlinks */
  if (ifa->type != OSPF_IT_VLINK)
  {
    WALK_LIST(iff, po->iface_list)
    {
      if ((iff->type == OSPF_IT_VLINK) && (iff->iface == ifa->iface))
        ospf_iface_down(iff);
    }
  }

  WALK_LIST_DELSAFE(n, nx, ifa->neigh_list)
  {
    OSPF_TRACE(D_EVENTS, "Removing neighbor %I", n->ip);
    ospf_neigh_remove(n);
  }

  rfree(ifa->sk);
  ifa->sk = NULL;

  if (ifa->type == OSPF_IT_VLINK)
  {
    ifa->iface = NULL;
    return;
  }
  else
  {
    rfree(ifa->wait_timer);
    rfree(ifa->hello_timer);
    rfree(ifa->poll_timer);
    rfree(ifa->lock);
    rem_node(NODE ifa);
    mb_free(ifa);
  }
}

/**
 * ospf_iface_sm - OSPF interface state machine
 * @ifa: OSPF interface
 * @event: event comming to state machine
 *
 * This fully respects 9.3 of RFC 2328 except we don't use %LOOP state of
 * interface.
 */
void
ospf_iface_sm(struct ospf_iface *ifa, int event)
{
  struct ospf_area *oa = ifa->oa;

  DBG("SM on %s %s. Event is '%s'\n", (ifa->type == OSPF_IT_VLINK) ? "vlink" : "iface",
    ifa->iface ? ifa->iface->name : "(none)" , ospf_ism[event]);

  switch (event)
  {
  case ISM_UP:
    if (ifa->state == OSPF_IS_DOWN)
    {
      /* Now, nothing should be adjacent */
      if ((ifa->type == OSPF_IT_PTP) || (ifa->type == OSPF_IT_VLINK))
      {
	ospf_iface_chstate(ifa, OSPF_IS_PTP);
      }
      else
      {
	if (ifa->priority == 0)
	  ospf_iface_chstate(ifa, OSPF_IS_DROTHER);
	else
	{
	  ospf_iface_chstate(ifa, OSPF_IS_WAITING);
	  tm_start(ifa->wait_timer, ifa->waitint);
	}
      }

      tm_start(ifa->hello_timer, ifa->helloint);

      if (ifa->poll_timer)
	tm_start(ifa->poll_timer, ifa->pollint);

      hello_timer_hook(ifa->hello_timer);
    }
    schedule_link_lsa(ifa);
    schedule_rt_lsa(ifa->oa);
    break;
  case ISM_BACKS:
  case ISM_WAITF:
    if (ifa->state == OSPF_IS_WAITING)
    {
      bdr_election(ifa);
    }
    break;
  case ISM_NEICH:
    if ((ifa->state == OSPF_IS_DROTHER) || (ifa->state == OSPF_IS_DR) ||
	(ifa->state == OSPF_IS_BACKUP))
    {
      bdr_election(ifa);
      schedule_rt_lsa(ifa->oa);
    }
    break;
  case ISM_DOWN:
    ospf_iface_chstate(ifa, OSPF_IS_DOWN);
    ospf_iface_down(ifa);
    schedule_link_lsa(ifa);
    schedule_rt_lsa(oa);
    break;
  case ISM_LOOP:		/* Useless? */
    ospf_iface_chstate(ifa, OSPF_IS_LOOP);
    ospf_iface_down(ifa);
    schedule_rt_lsa(ifa->oa);
    break;
  case ISM_UNLOOP:
    ospf_iface_chstate(ifa, OSPF_IS_DOWN);
    schedule_rt_lsa(ifa->oa);
    break;
  default:
    bug("OSPF_I_SM - Unknown event?");
    break;
  }

}

#if 0
static sock *
ospf_open_mc_socket(struct ospf_iface *ifa)
{
  sock *mcsk;
  struct proto *p = &ifa->oa->po->proto;

  mcsk = sk_new(p->pool);
  mcsk->type = SK_IP_MC;
  mcsk->sport = 0;
  mcsk->dport = OSPF_PROTO;

#ifdef OSPFv2
  mcsk->saddr = AllSPFRouters;
#else /* OSPFv3 */
  // mcsk->saddr = AllSPFRouters;
  mcsk->saddr = ifa->lladdr;
#endif

  mcsk->daddr = AllSPFRouters;
  mcsk->tos = IP_PREC_INTERNET_CONTROL;
  mcsk->ttl = 1;
  mcsk->rx_hook = ospf_rx_hook;
  mcsk->tx_hook = ospf_tx_hook;
  mcsk->err_hook = ospf_err_hook;
  mcsk->iface = ifa->iface;
  mcsk->rbsize = rxbufsize(ifa);
  mcsk->tbsize = ifa->iface->mtu;
  mcsk->data = (void *) ifa;
  if (sk_open(mcsk) != 0)
  {
    DBG("%s: SK_OPEN: mc open failed.\n", p->name);
    return (NULL);
  }
  DBG("%s: SK_OPEN: mc opened.\n", p->name);
  return (mcsk);
}
#endif

u8
ospf_iface_clasify(struct iface * ifa)
{
  if (ifa->addr->flags & IA_UNNUMBERED)
    return OSPF_IT_PTP;

  if ((ifa->flags & (IF_MULTIACCESS | IF_MULTICAST)) ==
      (IF_MULTIACCESS | IF_MULTICAST))
    return OSPF_IT_BCAST;

  if ((ifa->flags & (IF_MULTIACCESS | IF_MULTICAST)) == IF_MULTIACCESS)
    return OSPF_IT_NBMA;

  return OSPF_IT_PTP;
}

struct ospf_iface *
ospf_iface_find(struct proto_ospf *p, struct iface *what)
{
  struct ospf_iface *i;

  WALK_LIST(i, p->iface_list) if ((i->iface == what) && (i->type != OSPF_IT_VLINK))
    return i;
  return NULL;
}

static void
ospf_iface_add(struct object_lock *lock)
{
  struct ospf_iface *ifa = lock->data;
  struct proto_ospf *po = ifa->oa->po;
  struct proto *p = &po->proto;
  struct iface *iface = lock->iface;

  ifa->lock = lock;

  ifa->ioprob = OSPF_I_OK;

  ifa->sk = ospf_open_socket(ifa, ifa->type != OSPF_IT_NBMA);
  if (ifa->sk == NULL)
  {
    log("%s: Huh? could not open ip socket on interface %s?", p->name,
	iface->name);
    log("%s: Declaring as stub.", p->name);
    ifa->stub = 1;
    ifa->ioprob += OSPF_I_IP;
  }

  ifa->state = OSPF_IS_DOWN;
  ospf_iface_sm(ifa, ISM_UP);
}

void
ospf_iface_new(struct proto_ospf *po, struct iface *iface,
	       struct ospf_area_config *ac, struct ospf_iface_patt *ip)
{
  struct proto *p = &po->proto;
  struct ospf_iface *ifa;
  struct nbma_node *nbma, *nb;
  struct object_lock *lock;
  struct ospf_area *oa;

  ifa = mb_allocz(p->pool, sizeof(struct ospf_iface));
  ifa->iface = iface;

  ifa->cost = ip->cost;
  ifa->rxmtint = ip->rxmtint;
  ifa->inftransdelay = ip->inftransdelay;
  ifa->priority = ip->priority;
  ifa->helloint = ip->helloint;
  ifa->pollint = ip->pollint;
  ifa->strictnbma = ip->strictnbma;
  ifa->waitint = ip->waitint;
  ifa->dead = (ip->dead == 0) ? ip->deadc * ifa->helloint : ip->dead;
  ifa->stub = ip->stub;

#ifdef OSPFv2
  ifa->autype = ip->autype;
  ifa->passwords = ip->passwords;
#endif

#ifdef OSPFv3
  ifa->instance_id = ip->instance_id;

  ifa->lladdr = IPA_NONE;

  /* Find link-local address */
  if (ifa->type != OSPF_IT_VLINK)
    {
      struct ifa *a;
      WALK_LIST(a, iface->addrs)
	if (a->scope == SCOPE_LINK)
	  {
	    ifa->lladdr = a->ip;
	    break;
	  }

      if (! ipa_nonzero(ifa->lladdr))
	log(L_WARN "%s: Missing link local address on interface %s", p->name,  iface->name);
    }
#endif

  ifa->rxbuf = ip->rxbuf;

  if (ip->type == OSPF_IT_UNDEF)
    ifa->type = ospf_iface_clasify(ifa->iface);
  else
    ifa->type = ip->type;

#ifdef OSPFv2
  if ((ifa->type != OSPF_IT_PTP) && (ifa->type != OSPF_IT_VLINK) &&
      (ifa->iface->addr->flags & IA_UNNUMBERED))
  {
    log(L_WARN "%s: Missing proper IP prefix on interface %s, forcing point-to-point mode",
	p->name,  iface->name);
    ifa->type = OSPF_IT_PTP;
  }
#endif

  init_list(&ifa->neigh_list);
  init_list(&ifa->nbma_list);

  WALK_LIST(nb, ip->nbma_list)
  {
    nbma = mb_alloc(p->pool, sizeof(struct nbma_node));
    nbma->ip = nb->ip;
    nbma->eligible = nb->eligible;
    add_tail(&ifa->nbma_list, NODE nbma);
  }

  /* Add hello timer */
  ifa->hello_timer = tm_new(p->pool);
  ifa->hello_timer->data = ifa;
  ifa->hello_timer->randomize = 0;
  ifa->hello_timer->hook = hello_timer_hook;
  ifa->hello_timer->recurrent = ifa->helloint;
  DBG("%s: Installing hello timer. (%u)\n", p->name, ifa->helloint);

  if (ifa->type == OSPF_IT_NBMA)
  {
    ifa->poll_timer = tm_new(p->pool);
    ifa->poll_timer->data = ifa;
    ifa->poll_timer->randomize = 0;
    ifa->poll_timer->hook = poll_timer_hook;
    ifa->poll_timer->recurrent = ifa->pollint;
    DBG("%s: Installing poll timer. (%u)\n", p->name, ifa->pollint);
  }
  else
    ifa->poll_timer = NULL;

  ifa->wait_timer = tm_new(p->pool);
  ifa->wait_timer->data = ifa;
  ifa->wait_timer->randomize = 0;
  ifa->wait_timer->hook = wait_timer_hook;
  ifa->wait_timer->recurrent = 0;
  DBG("%s: Installing wait timer. (%u)\n", p->name, ifa->waitint);
  add_tail(&((struct proto_ospf *) p)->iface_list, NODE ifa);
  ifa->state = OSPF_IS_DOWN;

  ifa->oa = NULL;
  WALK_LIST(oa, po->area_list)
  {
    if (oa->areaid == ac->areaid)
    {
      ifa->oa = oa;
      break;
    }
  }

  if (!ifa->oa)
    bug("Cannot add any area to accepted Interface");
  else

  if (ifa->type == OSPF_IT_VLINK)
  {
    ifa->oa = po->backbone;
    ifa->voa = oa;
    ifa->vid = ip->vid;
    return;			/* Don't lock, don't add sockets */
  }

  lock = olock_new(p->pool);
  lock->addr = AllSPFRouters;
  lock->type = OBJLOCK_IP;
  lock->port = OSPF_PROTO;
  lock->iface = iface;
  lock->data = ifa;
  lock->hook = ospf_iface_add;

  olock_acquire(lock);
}

void
ospf_iface_change_mtu(struct proto_ospf *po, struct ospf_iface *ifa)
{
  struct proto *p = &po->proto;
  struct ospf_packet *op;
  struct ospf_neighbor *n;
  OSPF_TRACE(D_EVENTS, "Changing MTU on interface %s.", ifa->iface->name);

  if (ifa->sk)
  {
    ifa->sk->rbsize = rxbufsize(ifa);
    ifa->sk->tbsize = ifa->iface->mtu;
    sk_reallocate(ifa->sk);
  }

  WALK_LIST(n, ifa->neigh_list)
  {
    op = (struct ospf_packet *) n->ldbdes;
    n->ldbdes = mb_allocz(n->pool, ifa->iface->mtu);

    if (ntohs(op->length) <= ifa->iface->mtu)	/* If the packet in old buffer is bigger, let it filled by zeros */
      memcpy(n->ldbdes, op, ifa->iface->mtu);	/* If the packet is old is same or smaller, copy it */

    rfree(op);
  }
}

void
ospf_iface_notify(struct proto *p, unsigned flags, struct iface *iface)
{
  struct proto_ospf *po = (struct proto_ospf *) p;
  struct ospf_config *c = (struct ospf_config *) (p->cf);
  struct ospf_area_config *ac;
  struct ospf_iface_patt *ip = NULL;
  struct ospf_iface *ifa;

  DBG("%s: If notify called\n", p->name);
  if (iface->flags & IF_IGNORE)
    return;

  if (flags & IF_CHANGE_UP)
  {
    WALK_LIST(ac, c->area_list)
    {
      if (ip = (struct ospf_iface_patt *)
	  iface_patt_find(&ac->patt_list, iface))
	break;
    }

    if (ip)
    {
      OSPF_TRACE(D_EVENTS, "Using interface %s.", iface->name);
      ospf_iface_new(po, iface, ac, ip);
    }
  }

  if (flags & IF_CHANGE_DOWN)
  {
    if ((ifa = ospf_iface_find((struct proto_ospf *) p, iface)) != NULL)
    {
      OSPF_TRACE(D_EVENTS, "Killing interface %s.", iface->name);
      ospf_iface_sm(ifa, ISM_DOWN);
    }
  }

  if (flags & IF_CHANGE_MTU)
  {
    if ((ifa = ospf_iface_find((struct proto_ospf *) p, iface)) != NULL)
      ospf_iface_change_mtu(po, ifa);
  }
}

void
ospf_iface_info(struct ospf_iface *ifa)
{
  char *strict = "(strict)";

  if ((ifa->type != OSPF_IT_NBMA) || (ifa->strictnbma == 0))
    strict = "";
  if (ifa->type == OSPF_IT_VLINK)
  {
    cli_msg(-1015, "Virtual link to %R:", ifa->vid);
    cli_msg(-1015, "\tPeer IP: %I", ifa->vip);
    cli_msg(-1015, "\tTransit area: %R (%u)", ifa->voa->areaid,
	    ifa->voa->areaid);
    cli_msg(-1015, "\tInterface: \"%s\"",
	    (ifa->iface ? ifa->iface->name : "(none)"));
  }
  else
  {
    cli_msg(-1015, "Interface \"%s\":",
	    (ifa->iface ? ifa->iface->name : "(none)"));
    cli_msg(-1015, "\tType: %s %s", ospf_it[ifa->type], strict);
    cli_msg(-1015, "\tArea: %R (%u)", ifa->oa->areaid, ifa->oa->areaid);
  }
  cli_msg(-1015, "\tState: %s %s", ospf_is[ifa->state],
	  ifa->stub ? "(stub)" : "");
  cli_msg(-1015, "\tPriority: %u", ifa->priority);
  cli_msg(-1015, "\tCost: %u", ifa->cost);
  cli_msg(-1015, "\tHello timer: %u", ifa->helloint);

  if (ifa->type == OSPF_IT_NBMA)
  {
    cli_msg(-1015, "\tPoll timer: %u", ifa->pollint);
  }
  cli_msg(-1015, "\tWait timer: %u", ifa->waitint);
  cli_msg(-1015, "\tDead timer: %u", ifa->dead);
  cli_msg(-1015, "\tRetransmit timer: %u", ifa->rxmtint);
  if ((ifa->type == OSPF_IT_BCAST) || (ifa->type == OSPF_IT_NBMA))
  {
    cli_msg(-1015, "\tDesigned router (ID): %R", ifa->drid);
    cli_msg(-1015, "\tDesigned router (IP): %I", ifa->drip);
    cli_msg(-1015, "\tBackup designed router (ID): %R", ifa->bdrid);
    cli_msg(-1015, "\tBackup designed router (IP): %I", ifa->bdrip);
  }
}

void
ospf_iface_shutdown(struct ospf_iface *ifa)
{
  init_list(&ifa->neigh_list);
  hello_timer_hook(ifa->hello_timer);
}

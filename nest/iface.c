/*
 *	BIRD -- Management of Interfaces and Neighbor Cache
 *
 *	(c) 1998--2000 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

/**
 * DOC: Interfaces
 *
 * The interface module keeps track of all network interfaces in the
 * system and their addresses.
 *
 * Each interface is represented by an &iface structure which carries
 * interface capability flags (%IF_MULTIACCESS, %IF_BROADCAST etc.),
 * MTU, interface name and index and finally a linked list of network
 * prefixes assigned to the interface, each one represented by
 * struct &ifa.
 *
 * The interface module keeps a `soft-up' state for each &iface which
 * is a conjunction of link being up, the interface being of a `sane'
 * type and at least one IP address assigned to it.
 */

#undef LOCAL_DEBUG

#include "nest/bird.h"
#include "nest/iface.h"
#include "nest/protocol.h"
#include "nest/cli.h"
#include "lib/resource.h"
#include "lib/string.h"
#include "conf/conf.h"

static pool *if_pool;

static void auto_router_id(void);

list iface_list;

/**
 * ifa_dump - dump interface address
 * @a: interface address descriptor
 *
 * This function dumps contents of an &ifa to the debug output.
 */
void
ifa_dump(struct ifa *a)
{
  debug("\t%I, net %I/%-2d bc %I -> %I%s%s%s\n", a->ip, a->prefix, a->pxlen, a->brd, a->opposite,
	(a->flags & IF_UP) ? "" : " DOWN",
	(a->flags & IA_PRIMARY) ? "" : " SEC",
	(a->flags & IA_UNNUMBERED) ? " UNNUM" : "");
}

/**
 * if_dump - dump interface
 * @i: interface to dump
 *
 * This function dumps all information associated with a given
 * network interface to the debug output.
 */
void
if_dump(struct iface *i)
{
  struct ifa *a;

  debug("IF%d: %s", i->index, i->name);
  if (i->flags & IF_ADMIN_DOWN)
    debug(" ADMIN-DOWN");
  if (i->flags & IF_UP)
    debug(" UP");
  else
    debug(" DOWN");
  if (i->flags & IF_LINK_UP)
    debug(" LINK-UP");
  if (i->flags & IF_MULTIACCESS)
    debug(" MA");
  if (i->flags & IF_BROADCAST)
    debug(" BC");
  if (i->flags & IF_MULTICAST)
    debug(" MC");
  if (i->flags & IF_LOOPBACK)
    debug(" LOOP");
  if (i->flags & IF_IGNORE)
    debug(" IGN");
  if (i->flags & IF_TMP_DOWN)
    debug(" TDOWN");
  debug(" MTU=%d\n", i->mtu);
  WALK_LIST(a, i->addrs)
    {
      ifa_dump(a);
      ASSERT((a != i->addr) == !(a->flags & IA_PRIMARY));
    }
}

/**
 * if_dump_all - dump all interfaces
 *
 * This function dumps information about all known network
 * interfaces to the debug output.
 */
void
if_dump_all(void)
{
  struct iface *i;

  debug("Known network interfaces:\n");
  WALK_LIST(i, iface_list)
    if_dump(i);
  debug("Router ID: %08x\n", config->router_id);
}

static inline unsigned
if_what_changed(struct iface *i, struct iface *j)
{
  unsigned c;

  if (((i->flags ^ j->flags) & ~(IF_UP | IF_ADMIN_DOWN | IF_UPDATED | IF_LINK_UP | IF_TMP_DOWN | IF_JUST_CREATED))
      || i->index != j->index)
    return IF_CHANGE_TOO_MUCH;
  c = 0;
  if ((i->flags ^ j->flags) & IF_UP)
    c |= (i->flags & IF_UP) ? IF_CHANGE_DOWN : IF_CHANGE_UP;
  if (i->mtu != j->mtu)
    c |= IF_CHANGE_MTU;
  return c;
}

static inline void
if_copy(struct iface *to, struct iface *from)
{
  to->flags = from->flags | (to->flags & IF_TMP_DOWN);
  to->mtu = from->mtu;
}

static inline void
ifa_send_notify(struct proto *p, unsigned c, struct ifa *a)
{
  if (p->ifa_notify)
    {
      if (p->debug & D_IFACES)
	log(L_TRACE "%s < %s address %I/%d on interface %s %s",
	    p->name, (a->flags & IA_PRIMARY) ? "primary" : "secondary",
	    a->prefix, a->pxlen, a->iface->name,
	    (c & IF_CHANGE_UP) ? "added" : "removed");
      p->ifa_notify(p, c, a);
    }
}

static void
ifa_notify_change(unsigned c, struct ifa *a)
{
  struct proto *p;

  DBG("IFA change notification (%x) for %s:%I\n", c, a->iface->name, a->ip);
  WALK_LIST(p, active_proto_list)
    ifa_send_notify(p, c, a);
}

static inline void
if_send_notify(struct proto *p, unsigned c, struct iface *i)
{
  if (p->if_notify)
    {
      if (p->debug & D_IFACES)
	log(L_TRACE "%s < interface %s %s", p->name, i->name,
	    (c & IF_CHANGE_UP) ? "goes up" :
	    (c & IF_CHANGE_DOWN) ? "goes down" :
	    (c & IF_CHANGE_MTU) ? "changes MTU" :
	    (c & IF_CHANGE_CREATE) ? "created" :
	    "sends unknown event");
      p->if_notify(p, c, i);
    }
}

static void
if_notify_change(unsigned c, struct iface *i)
{
  struct proto *p;
  struct ifa *a;

  if (i->flags & IF_JUST_CREATED)
    {
      i->flags &= ~IF_JUST_CREATED;
      c |= IF_CHANGE_CREATE | IF_CHANGE_MTU;
    }

  DBG("Interface change notification (%x) for %s\n", c, i->name);
#ifdef LOCAL_DEBUG
  if_dump(i);
#endif

  if (c & IF_CHANGE_UP)
    neigh_if_up(i);
  if (c & IF_CHANGE_DOWN)
    WALK_LIST(a, i->addrs)
      {
	a->flags = (i->flags & ~IA_FLAGS) | (a->flags & IA_FLAGS);
	ifa_notify_change(IF_CHANGE_DOWN, a);
      }

  WALK_LIST(p, active_proto_list)
    if_send_notify(p, c, i);

  if (c & IF_CHANGE_UP)
    WALK_LIST(a, i->addrs)
      {
	a->flags = (i->flags & ~IA_FLAGS) | (a->flags & IA_FLAGS);
	ifa_notify_change(IF_CHANGE_UP, a);
      }
  if (c & IF_CHANGE_DOWN)
    neigh_if_down(i);
}

static unsigned
if_recalc_flags(struct iface *i, unsigned flags)
{
  if ((flags & (IF_ADMIN_DOWN | IF_TMP_DOWN)) ||
      !(flags & IF_LINK_UP) ||
      !i->addr)
    flags &= ~IF_UP;
  else
    flags |= IF_UP;
  return flags;
}

static void
if_change_flags(struct iface *i, unsigned flags)
{
  unsigned of = i->flags;

  i->flags = if_recalc_flags(i, flags);
  if ((i->flags ^ of) & IF_UP)
    if_notify_change((i->flags & IF_UP) ? IF_CHANGE_UP : IF_CHANGE_DOWN, i);
}

/**
 * if_update - update interface status
 * @new: new interface status
 *
 * if_update() is called by the low-level platform dependent code
 * whenever it notices an interface change.
 *
 * There exist two types of interface updates -- synchronous and asynchronous
 * ones. In the synchronous case, the low-level code calls if_start_update(),
 * scans all interfaces reported by the OS, uses if_update() and ifa_update()
 * to pass them to the core and then it finishes the update sequence by
 * calling if_end_update(). When working asynchronously, the sysdep code
 * calls if_update() and ifa_update() whenever it notices a change.
 *
 * if_update() will automatically notify all other modules about the change.
 */
struct iface *
if_update(struct iface *new)
{
  struct iface *i;
  unsigned c;

  WALK_LIST(i, iface_list)
    if (!strcmp(new->name, i->name))
      {
	new->addr = i->addr;
	new->flags = if_recalc_flags(new, new->flags);
	c = if_what_changed(i, new);
	if (c & IF_CHANGE_TOO_MUCH)	/* Changed a lot, convert it to down/up */
	  {
	    DBG("Interface %s changed too much -- forcing down/up transition\n", i->name);
	    if_change_flags(i, i->flags | IF_TMP_DOWN);
	    rem_node(&i->n);
	    new->addr = i->addr;
	    memcpy(&new->addrs, &i->addrs, sizeof(i->addrs));
	    memcpy(i, new, sizeof(*i));
	    goto newif;
	  }
	else if (c)
	  {
	    if_copy(i, new);
	    if_notify_change(c, i);
	  }
	i->flags |= IF_UPDATED;
	return i;
      }
  i = mb_alloc(if_pool, sizeof(struct iface));
  memcpy(i, new, sizeof(*i));
  init_list(&i->addrs);
newif:
  init_list(&i->neighbors);
  i->flags |= IF_UPDATED | IF_TMP_DOWN;		/* Tmp down as we don't have addresses yet */
  add_tail(&iface_list, &i->n);
  return i;
}

void
if_start_update(void)
{
  struct iface *i;
  struct ifa *a;

  WALK_LIST(i, iface_list)
    {
      i->flags &= ~IF_UPDATED;
      WALK_LIST(a, i->addrs)
	a->flags &= ~IF_UPDATED;
    }
}

void
if_end_partial_update(struct iface *i)
{
  if (i->flags & IF_TMP_DOWN)
    if_change_flags(i, i->flags & ~IF_TMP_DOWN);
}

void
if_end_update(void)
{
  struct iface *i;
  struct ifa *a, *b;

  if (!config->router_id)
    auto_router_id();

  WALK_LIST(i, iface_list)
    {
      if (!(i->flags & IF_UPDATED))
	if_change_flags(i, (i->flags & ~IF_LINK_UP) | IF_ADMIN_DOWN);
      else
	{
	  WALK_LIST_DELSAFE(a, b, i->addrs)
	    if (!(a->flags & IF_UPDATED))
	      ifa_delete(a);
	  if_end_partial_update(i);
	}
    }
}

/**
 * if_feed_baby - advertise interfaces to a new protocol
 * @p: protocol to feed
 *
 * When a new protocol starts, this function sends it a series
 * of notifications about all existing interfaces.
 */
void
if_feed_baby(struct proto *p)
{
  struct iface *i;
  struct ifa *a;

  if (!p->if_notify && !p->ifa_notify)	/* shortcut */
    return;
  DBG("Announcing interfaces to new protocol %s\n", p->name);
  WALK_LIST(i, iface_list)
    {
      if_send_notify(p, IF_CHANGE_CREATE | ((i->flags & IF_UP) ? IF_CHANGE_UP : 0), i);
      if (i->flags & IF_UP)
	WALK_LIST(a, i->addrs)
	  ifa_send_notify(p, IF_CHANGE_CREATE | IF_CHANGE_UP, a);
    }
}

/**
 * if_find_by_index - find interface by ifindex
 * @idx: ifindex
 *
 * This function finds an &iface structure corresponding to an interface
 * of the given index @idx. Returns a pointer to the structure or %NULL
 * if no such structure exists.
 */
struct iface *
if_find_by_index(unsigned idx)
{
  struct iface *i;

  WALK_LIST(i, iface_list)
    if (i->index == idx)
      return i;
  return NULL;
}

/**
 * if_find_by_name - find interface by name
 * @name: interface name
 *
 * This function finds an &iface structure corresponding to an interface
 * of the given name @name. Returns a pointer to the structure or %NULL
 * if no such structure exists.
 */
struct iface *
if_find_by_name(char *name)
{
  struct iface *i;

  WALK_LIST(i, iface_list)
    if (!strcmp(i->name, name))
      return i;
  return NULL;
}

static int
ifa_recalc_primary(struct iface *i)
{
  struct ifa *a, *b = NULL;
  int res;

  WALK_LIST(a, i->addrs)
    {
      if (!(a->flags & IA_SECONDARY) && (!b || a->scope > b->scope))
	b = a;
      a->flags &= ~IA_PRIMARY;
    }
  res = (b != i->addr);
  i->addr = b;
  if (b)
    {
      b->flags |= IA_PRIMARY;
      rem_node(&b->n);
      add_head(&i->addrs, &b->n);
    }
  return res;
}

/**
 * ifa_update - update interface address
 * @a: new interface address
 *
 * This function adds address information to a network
 * interface. It's called by the platform dependent code during
 * the interface update process described under if_update().
 */
struct ifa *
ifa_update(struct ifa *a)
{
  struct iface *i = a->iface;
  struct ifa *b;

  WALK_LIST(b, i->addrs)
    if (ipa_equal(b->ip, a->ip))
      {
	if (ipa_equal(b->prefix, a->prefix) &&
	    b->pxlen == a->pxlen &&
	    ipa_equal(b->brd, a->brd) &&
	    ipa_equal(b->opposite, a->opposite) &&
	    b->scope == a->scope &&
	    !((b->flags ^ a->flags) & IA_UNNUMBERED))
	  {
	    b->flags |= IF_UPDATED;
	    return b;
	  }
	ifa_delete(b);
	break;
      }

  if (!(i->flags & IF_MULTIACCESS) && a->pxlen < BITS_PER_IP_ADDRESS - 2)
    log(L_WARN "Strange prefix length %d for point-to-point interface %s", a->pxlen, i->name);
#ifndef IPV6
  if ((i->flags & IF_BROADCAST) && !ipa_nonzero(a->brd))
    log(L_ERR "Missing broadcast address for interface %s", i->name);
#endif

  b = mb_alloc(if_pool, sizeof(struct ifa));
  memcpy(b, a, sizeof(struct ifa));
  add_tail(&i->addrs, &b->n);
  b->flags = (i->flags & ~IA_FLAGS) | (a->flags & IA_FLAGS);
  if ((!i->addr || i->addr->scope < b->scope) && ifa_recalc_primary(i))
    if_change_flags(i, i->flags | IF_TMP_DOWN);
  if (b->flags & IF_UP)
    ifa_notify_change(IF_CHANGE_CREATE | IF_CHANGE_UP, b);
  return b;
}

/**
 * ifa_delete - remove interface address
 * @a: interface address
 *
 * This function removes address information from a network
 * interface. It's called by the platform dependent code during
 * the interface update process described under if_update().
 */
void
ifa_delete(struct ifa *a)
{
  struct iface *i = a->iface;
  struct ifa *b;

  WALK_LIST(b, i->addrs)
    if (ipa_equal(b->ip, a->ip))
      {
	rem_node(&b->n);
	if (b->flags & IF_UP)
	  {
	    b->flags &= ~IF_UP;
	    ifa_notify_change(IF_CHANGE_DOWN, b);
	  }
	if (b->flags & IA_PRIMARY)
	  {
	    if_change_flags(i, i->flags | IF_TMP_DOWN);
	    ifa_recalc_primary(i);
	  }
	mb_free(b);
	return;
      }
}

static void
auto_router_id(void)
{
#ifndef IPV6
  struct iface *i, *j;

  j = NULL;
  WALK_LIST(i, iface_list)
    if ((i->flags & IF_LINK_UP) &&
	!(i->flags & (IF_IGNORE | IF_ADMIN_DOWN)) &&
	i->addr &&
	!(i->addr->flags & IA_UNNUMBERED) &&
	(!j || ipa_to_u32(i->addr->ip) < ipa_to_u32(j->addr->ip)))
      j = i;
  if (!j)
    die("Cannot determine router ID (no suitable network interface found), please configure it manually");
  log(L_INFO "Guessed router ID %I according to interface %s", j->addr->ip, j->name);
  config->router_id = ipa_to_u32(j->addr->ip);
#endif
}

/**
 * if_init - initialize interface module
 *
 * This function is called during BIRD startup to initialize
 * all data structures of the interface module.
 */
void
if_init(void)
{
  if_pool = rp_new(&root_pool, "Interfaces");
  init_list(&iface_list);
  neigh_init(if_pool);
}

/*
 *	Interface Pattern Lists
 */

static int
iface_patt_match(struct iface_patt *ifp, struct iface *i)
{
  struct iface_patt_node *p;

  WALK_LIST(p, ifp->ipn_list)
    {
      char *t = p->pattern;
      int pos = p->positive;

      if (t)
	{
	  if (*t == '-')
	    {
	      t++;
	      pos = !pos;
	    }

	  if (!patmatch(t, i->name))
	    continue;
	}

      if (p->pxlen)
	if (!i->addr || !ipa_in_net(i->addr->ip, p->prefix, p->pxlen))
	  continue;

      return pos;
    }

  return 0;
}

struct iface_patt *
iface_patt_find(list *l, struct iface *i)
{
  struct iface_patt *p;

  WALK_LIST(p, *l)
    if (iface_patt_match(p, i))
      return p;

  return NULL;
}

static int
iface_plists_equal(struct iface_patt *pa, struct iface_patt *pb)
{
  struct iface_patt_node *x, *y;

  x = HEAD(pa->ipn_list);
  y = HEAD(pb->ipn_list);
  while (x->n.next && y->n.next)
    {
      if ((x->positive != y->positive) ||
	  (!x->pattern && y->pattern) ||	/* This nasty lines where written by me... :-( Feela */
	  (!y->pattern && x->pattern) ||
	  ((x->pattern != y->pattern) && strcmp(x->pattern, y->pattern)) ||
	  !ipa_equal(x->prefix, y->prefix) ||
	  (x->pxlen != y->pxlen))
	return 0;
      x = (void *) x->n.next;
      y = (void *) y->n.next;
    }
  return (!x->n.next && !y->n.next);
}

int
iface_patts_equal(list *a, list *b, int (*comp)(struct iface_patt *, struct iface_patt *))
{
  struct iface_patt *x, *y;

  x = HEAD(*a);
  y = HEAD(*b);
  while (x->n.next && y->n.next)
    {
      if (!iface_plists_equal(x, y) ||
	  (comp && !comp(x, y)))
	return 0;
      x = (void *) x->n.next;
      y = (void *) y->n.next;
    }
  return (!x->n.next && !y->n.next);
}

/*
 *  CLI commands.
 */

static void
if_show_addr(struct ifa *a)
{
  byte broad[STD_ADDRESS_P_LENGTH + 16];
  byte opp[STD_ADDRESS_P_LENGTH + 16];

  if (ipa_nonzero(a->brd))
    bsprintf(broad, ", broadcast %I", a->brd);
  else
    broad[0] = 0;
  if (ipa_nonzero(a->opposite))
    bsprintf(opp, ", opposite %I", a->opposite);
  else
    opp[0] = 0;
  cli_msg(-1003, "\t%I/%d (%s%s%s, scope %s%s)",
	  a->ip, a->pxlen,
	  (a->flags & IA_PRIMARY) ? "Primary" : (a->flags & IA_SECONDARY) ? "Secondary" : "Unselected",
	  broad, opp,
	  ip_scope_text(a->scope),
	  (a->flags & IA_UNNUMBERED) ? ", unnumbered" : "");
}

void
if_show(void)
{
  struct iface *i;
  struct ifa *a;
  char *type;

  WALK_LIST(i, iface_list)
    {
      cli_msg(-1001, "%s %s (index=%d)", i->name, (i->flags & IF_UP) ? "up" : "DOWN", i->index);
      if (!(i->flags & IF_MULTIACCESS))
	type = "PtP";
      else
	type = "MultiAccess";
      cli_msg(-1004, "\t%s%s%s Admin%s Link%s%s%s MTU=%d",
	      type,
	      (i->flags & IF_BROADCAST) ? " Broadcast" : "",
	      (i->flags & IF_MULTICAST) ? " Multicast" : "",
	      (i->flags & IF_ADMIN_DOWN) ? "Down" : "Up",
	      (i->flags & IF_LINK_UP) ? "Up" : "Down",
	      (i->flags & IF_LOOPBACK) ? " Loopback" : "",
	      (i->flags & IF_IGNORE) ? " Ignored" : "",
	      i->mtu);
      if (i->addr)
	if_show_addr(i->addr);
      WALK_LIST(a, i->addrs)
	if (a != i->addr)
	  if_show_addr(a);
    }
  cli_msg(0, "");
}

void
if_show_summary(void)
{
  struct iface *i;
  byte addr[STD_ADDRESS_P_LENGTH + 16];

  cli_msg(-2005, "interface state address");
  WALK_LIST(i, iface_list)
    {
      if (i->addr)
	bsprintf(addr, "%I/%d", i->addr->ip, i->addr->pxlen);
      else
	addr[0] = 0;
      cli_msg(-1005, "%-9s %-5s %s", i->name, (i->flags & IF_UP) ? "up" : "DOWN", addr);
    }
  cli_msg(0, "");
}

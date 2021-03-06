/*
 *	BIRD Internet Routing Daemon -- Linux Multicasting and Network Includes
 *
 *	(c) 1998--2000 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#include <net/if.h>

#ifdef IPV6

#ifndef IPV6_UNICAST_HOPS
/* Needed on glibc 2.0 systems */
#include <linux/in6.h>
#define CONFIG_IPV6_GLIBC_20
#endif

static inline void
set_inaddr(struct in6_addr *ia, ip_addr a)
{
  ipa_hton(a);
  memcpy(ia, &a, sizeof(a));
}

static inline void
get_inaddr(ip_addr *a, struct in6_addr *ia)
{
  memcpy(a, ia, sizeof(*a));
  ipa_ntoh(*a);
}

static inline char *
sysio_bind_to_iface(sock *s)
{
  struct ifreq ifr;
  strcpy(ifr.ifr_name, s->iface->name);
  if (setsockopt(s->fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0)
    return "SO_BINDTODEVICE";

  return NULL;
}

#else

static inline void
set_inaddr(struct in_addr *ia, ip_addr a)
{
  ipa_hton(a);
  memcpy(&ia->s_addr, &a, sizeof(a));
}

static inline void
get_inaddr(ip_addr *a, struct in_addr *ia)
{
  memcpy(a, &ia->s_addr, sizeof(*a));
  ipa_ntoh(*a);
}

/*
 *  Multicasting in Linux systems is a real mess. Not only different kernels
 *  have different interfaces, but also different libc's export it in different
 *  ways. Horrible.
 */


#if defined(CONFIG_LINUX_MC_MREQ) || defined(CONFIG_LINUX_MC_MREQ_BIND)
/*
 *  Older kernels support only struct mreq which matches interfaces by their
 *  addresses and thus fails on unnumbered devices. On newer 2.0 kernels
 *  we can use SO_BINDTODEVICE to circumvent this problem.
 */

#define MREQ_IFA struct in_addr
#define MREQ_GRP struct ip_mreq
static inline void fill_mreq_ifa(struct in_addr *m, struct iface *ifa UNUSED, ip_addr saddr, ip_addr maddr UNUSED)
{
  set_inaddr(m, saddr);
}

static inline void fill_mreq_grp(struct ip_mreq *m, struct iface *ifa, ip_addr saddr, ip_addr maddr)
{
  bzero(m, sizeof(*m));
#ifdef CONFIG_LINUX_MC_MREQ_BIND
  m->imr_interface.s_addr = INADDR_ANY;
#else
  set_inaddr(&m->imr_interface, saddr);
#endif
  set_inaddr(&m->imr_multiaddr, maddr);
}
#endif


#ifdef CONFIG_LINUX_MC_MREQN
/*
 *  2.1 and newer kernels use struct mreqn which passes ifindex, so no
 *  problems with unnumbered devices.
 */

#ifndef HAVE_STRUCT_IP_MREQN
/* Several versions of glibc don't define this structure, so we have to do it ourselves */
struct ip_mreqn
{
	struct in_addr	imr_multiaddr;		/* IP multicast address of group */
	struct in_addr	imr_address;		/* local IP address of interface */
	int		imr_ifindex;		/* Interface index */
};
#endif

#define MREQ_IFA struct ip_mreqn
#define MREQ_GRP struct ip_mreqn
#define fill_mreq_ifa fill_mreq
#define fill_mreq_grp fill_mreq

static inline void fill_mreq(struct ip_mreqn *m, struct iface *ifa, ip_addr saddr, ip_addr maddr)
{
  bzero(m, sizeof(*m));
  m->imr_ifindex = ifa->index;
  set_inaddr(&m->imr_address, saddr);
  set_inaddr(&m->imr_multiaddr, maddr);
}
#endif

static inline char *
sysio_setup_multicast(sock *s)
{
  MREQ_IFA m;
  int zero = 0;

  if (setsockopt(s->fd, SOL_IP, IP_MULTICAST_LOOP, &zero, sizeof(zero)) < 0)
    return "IP_MULTICAST_LOOP";

  if (setsockopt(s->fd, SOL_IP, IP_MULTICAST_TTL, &s->ttl, sizeof(s->ttl)) < 0)
    return "IP_MULTICAST_TTL";

  /* This defines where should we send _outgoing_ multicasts */
  fill_mreq_ifa(&m, s->iface, s->saddr, IPA_NONE);
  if (setsockopt(s->fd, SOL_IP, IP_MULTICAST_IF, &m, sizeof(m)) < 0)
    return "IP_MULTICAST_IF";

#if defined(CONFIG_LINUX_MC_MREQ_BIND) || defined(CONFIG_LINUX_MC_MREQN) 
  {
    struct ifreq ifr;
    strcpy(ifr.ifr_name, s->iface->name);
    if (setsockopt(s->fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0)
      return "SO_BINDTODEVICE";
  }
#endif

  return NULL;
}

static inline char *
sysio_join_group(sock *s, ip_addr maddr)
{
  MREQ_GRP m;

  /* And this one sets interface for _receiving_ multicasts from */
  fill_mreq_grp(&m, s->iface, s->saddr, maddr);
  if (setsockopt(s->fd, SOL_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0)
    return "IP_ADD_MEMBERSHIP";

  return NULL;
}

static inline char *
sysio_leave_group(sock *s, ip_addr maddr)
{
  MREQ_GRP m;

  /* And this one sets interface for _receiving_ multicasts from */
  fill_mreq_grp(&m, s->iface, s->saddr, maddr);
  if (setsockopt(s->fd, SOL_IP, IP_DROP_MEMBERSHIP, &m, sizeof(m)) < 0)
    return "IP_DROP_MEMBERSHIP";

  return NULL;
}

#endif


#include <linux/socket.h>
#include <linux/tcp.h>

/* For the case that we have older kernel headers */
/* Copied from Linux kernel file include/linux/tcp.h */

#ifndef TCP_MD5SIG

#define TCP_MD5SIG  14
#define TCP_MD5SIG_MAXKEYLEN 80

struct tcp_md5sig {
  struct  sockaddr_storage tcpm_addr;             /* address associated */
  __u16   __tcpm_pad1;                            /* zero */
  __u16   tcpm_keylen;                            /* key length */
  __u32   __tcpm_pad2;                            /* zero */
  __u8    tcpm_key[TCP_MD5SIG_MAXKEYLEN];         /* key (binary) */
};

#endif

static int
sk_set_md5_auth_int(sock *s, sockaddr *sa, char *passwd)
{
  struct tcp_md5sig md5;

  memset(&md5, 0, sizeof(md5));
  memcpy(&md5.tcpm_addr, (struct sockaddr *) sa, sizeof(*sa));

  if (passwd)
    {
      int len = strlen(passwd);

      if (len > TCP_MD5SIG_MAXKEYLEN)
	{
	  log(L_ERR "MD5 password too long");
	  return -1;
	}

      md5.tcpm_keylen = len;
      memcpy(&md5.tcpm_key, passwd, len);
    }

  int rv = setsockopt(s->fd, IPPROTO_TCP, TCP_MD5SIG, &md5, sizeof(md5));

  if (rv < 0) 
    {
      if (errno == ENOPROTOOPT)
	log(L_ERR "Kernel does not support TCP MD5 signatures");
      else
	log(L_ERR "sk_set_md5_auth_int: setsockopt: %m");
    }

  return rv;
}


#ifndef IPV6

/* RX/TX packet info handling for IPv4 */
/* Mostly similar to standardized IPv6 code */

#define CMSG_RX_SPACE CMSG_SPACE(sizeof(struct in_pktinfo))
#define CMSG_TX_SPACE CMSG_SPACE(sizeof(struct in_pktinfo))

static char *
sysio_register_cmsgs(sock *s)
{
  int ok = 1;
  if ((s->flags & SKF_LADDR_RX) &&
      setsockopt(s->fd, IPPROTO_IP, IP_PKTINFO, &ok, sizeof(ok)) < 0)
    return "IP_PKTINFO";

  return NULL;
}

static void
sysio_process_rx_cmsgs(sock *s, struct msghdr *msg)
{
  struct cmsghdr *cm;
  struct in_pktinfo *pi = NULL;

  if (!(s->flags & SKF_LADDR_RX))
    return;

  for (cm = CMSG_FIRSTHDR(msg); cm != NULL; cm = CMSG_NXTHDR(msg, cm))
    {
      if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO)
	pi = (struct in_pktinfo *) CMSG_DATA(cm);
    }

  if (!pi)
    {
      s->laddr = IPA_NONE;
      s->lifindex = 0;
      return;
    }

  get_inaddr(&s->laddr, &pi->ipi_addr);
  s->lifindex = pi->ipi_ifindex;
  return;
}

/*
static void
sysio_prepare_tx_cmsgs(sock *s, struct msghdr *msg, void *cbuf, size_t cbuflen)
{
  struct cmsghdr *cm;
  struct in_pktinfo *pi;

  if (!(s->flags & SKF_LADDR_TX))
    return;

  msg->msg_control = cbuf;
  msg->msg_controllen = cbuflen;

  cm = CMSG_FIRSTHDR(msg);
  cm->cmsg_level = IPPROTO_IP;
  cm->cmsg_type = IP_PKTINFO;
  cm->cmsg_len = CMSG_LEN(sizeof(*pi));

  pi = (struct in_pktinfo *) CMSG_DATA(cm);
  set_inaddr(&pi->ipi_spec_dst, s->saddr);
  pi->ipi_ifindex = s->iface ? s->iface->index : 0;

  msg->msg_controllen = cm->cmsg_len;
}
*/

#endif

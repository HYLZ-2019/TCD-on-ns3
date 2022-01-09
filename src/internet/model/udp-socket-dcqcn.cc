/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2007 INRIA
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 */

#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/inet-socket-address.h"
#include "ns3/inet6-socket-address.h"
#include "ns3/ipv4-route.h"
#include "ns3/ipv6-route.h"
#include "ns3/ipv4.h"
#include "ns3/ipv6.h"
#include "ns3/ipv6-l3-protocol.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv6-routing-protocol.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/ipv4-packet-info-tag.h"
#include "ns3/ipv6-packet-info-tag.h"
#include "ns3/error-model.h"
#include "ns3/flow-id-tag.h"
#include "ns3/udp-header.h"
#include "udp-socket-dcqcn.h"
#include "udp-l4-protocol.h"
#include "ipv4-end-point.h"
#include "ipv6-end-point.h"
#include <limits>
#define RDMA_RECV

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("UdpSocketDcqcn");

NS_OBJECT_ENSURE_REGISTERED (UdpSocketDcqcn);

// The correct maximum UDP message size is 65507, as determined by the following formula:
// 0xffff - (sizeof(IP Header) + sizeof(UDP Header)) = 65535-(20+8) = 65507
// \todo MAX_IPV4_UDP_DATAGRAM_SIZE is correct only for IPv4
static const uint32_t MAX_IPV4_UDP_DATAGRAM_SIZE = 65507; //!< Maximum UDP datagram size

// Add attributes generic to all UdpSockets to base class UdpSocket
TypeId
UdpSocketDcqcn::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::UdpSocketDcqcn")
    .SetParent<UdpSocket> ()
    .SetGroupName ("Internet")
    .AddConstructor<UdpSocketDcqcn> ()
    .AddTraceSource ("Drop",
                     "Drop UDP packet due to receive buffer overflow",
                     MakeTraceSourceAccessor (&UdpSocketDcqcn::m_dropTrace),
                     "ns3::Packet::TracedCallback")
    .AddAttribute ("IcmpCallback", "Callback invoked whenever an icmp error is received on this socket.",
                   CallbackValue (),
                   MakeCallbackAccessor (&UdpSocketDcqcn::m_icmpCallback),
                   MakeCallbackChecker ())
    .AddAttribute ("IcmpCallback6", "Callback invoked whenever an icmpv6 error is received on this socket.",
                   CallbackValue (),
                   MakeCallbackAccessor (&UdpSocketDcqcn::m_icmpCallback6),
                   MakeCallbackChecker ())
  ;
  return tid;
}

UdpSocketDcqcn::UdpSocketDcqcn ()
  : m_endPoint (0),
    m_endPoint6 (0),
    m_node (0),
    m_udp (0),
    m_errno (ERROR_NOTERROR),
    m_shutdownSend (false),
    m_shutdownRecv (false),
    m_connected (false),
    m_rxAvailable (0)
{
  NS_LOG_FUNCTION (this);
  m_allowBroadcast = false;
}

UdpSocketDcqcn::~UdpSocketDcqcn ()
{
  NS_LOG_FUNCTION (this);

  /// \todo  leave any multicast groups that have been joined
  m_node = 0;
  /**
   * Note: actually this function is called AFTER
   * UdpSocketDcqcn::Destroy or UdpSocketDcqcn::Destroy6
   * so the code below is unnecessary in normal operations
   */
  if (m_endPoint != 0)
    {
      NS_ASSERT (m_udp != 0);
      /**
       * Note that this piece of code is a bit tricky:
       * when DeAllocate is called, it will call into
       * Ipv4EndPointDemux::Deallocate which triggers
       * a delete of the associated endPoint which triggers
       * in turn a call to the method UdpSocketDcqcn::Destroy below
       * will will zero the m_endPoint field.
       */
      NS_ASSERT (m_endPoint != 0);
      m_udp->DeAllocate (m_endPoint);
      NS_ASSERT (m_endPoint == 0);
    }
  if (m_endPoint6 != 0)
    {
      NS_ASSERT (m_udp != 0);
      /**
       * Note that this piece of code is a bit tricky:
       * when DeAllocate is called, it will call into
       * Ipv4EndPointDemux::Deallocate which triggers
       * a delete of the associated endPoint which triggers
       * in turn a call to the method UdpSocketDcqcn::Destroy below
       * will will zero the m_endPoint field.
       */
      NS_ASSERT (m_endPoint6 != 0);
      m_udp->DeAllocate (m_endPoint6);
      NS_ASSERT (m_endPoint6 == 0);
    }
  m_udp = 0;
}

void 
UdpSocketDcqcn::SetNode (Ptr<Node> node)
{
  NS_LOG_FUNCTION (this << node);
  m_node = node;

}
void 
UdpSocketDcqcn::SetUdp (Ptr<UdpL4Protocol> udp)
{
  NS_LOG_FUNCTION (this << udp);
  m_udp = udp;
}


enum Socket::SocketErrno
UdpSocketDcqcn::GetErrno (void) const
{
  NS_LOG_FUNCTION (this);
  return m_errno;
}

enum Socket::SocketType
UdpSocketDcqcn::GetSocketType (void) const
{
  return NS3_SOCK_DGRAM;
}

Ptr<Node>
UdpSocketDcqcn::GetNode (void) const
{
  NS_LOG_FUNCTION (this);
  return m_node;
}

void 
UdpSocketDcqcn::Destroy (void)
{
  NS_LOG_FUNCTION (this);
  m_endPoint = 0;
}

void
UdpSocketDcqcn::Destroy6 (void)
{
  NS_LOG_FUNCTION (this);
  m_endPoint6 = 0;
}

/* Deallocate the end point and cancel all the timers */
void
UdpSocketDcqcn::DeallocateEndPoint (void)
{
  if (m_endPoint != 0)
    {
      m_endPoint->SetDestroyCallback (MakeNullCallback<void> ());
      m_udp->DeAllocate (m_endPoint);
      m_endPoint = 0;
    }
  if (m_endPoint6 != 0)
    {
      m_endPoint6->SetDestroyCallback (MakeNullCallback<void> ());
      m_udp->DeAllocate (m_endPoint6);
      m_endPoint6 = 0;
    }
}


int
UdpSocketDcqcn::FinishBind (void)
{
  NS_LOG_FUNCTION (this);
  bool done = false;
  if (m_endPoint != 0)
    {
      m_endPoint->SetRxCallback (MakeCallback (&UdpSocketDcqcn::ForwardUp, Ptr<UdpSocketDcqcn> (this)));
      m_endPoint->SetIcmpCallback (MakeCallback (&UdpSocketDcqcn::ForwardIcmp, Ptr<UdpSocketDcqcn> (this)));
      m_endPoint->SetDestroyCallback (MakeCallback (&UdpSocketDcqcn::Destroy, Ptr<UdpSocketDcqcn> (this)));
      done = true;
    }
  if (m_endPoint6 != 0)
    {
      m_endPoint6->SetRxCallback (MakeCallback (&UdpSocketDcqcn::ForwardUp6, Ptr<UdpSocketDcqcn> (this)));
      m_endPoint6->SetIcmpCallback (MakeCallback (&UdpSocketDcqcn::ForwardIcmp6, Ptr<UdpSocketDcqcn> (this)));
      m_endPoint6->SetDestroyCallback (MakeCallback (&UdpSocketDcqcn::Destroy6, Ptr<UdpSocketDcqcn> (this)));
      done = true;
    }
  if (done)
    {
      return 0;
    }
  return -1;
}

int
UdpSocketDcqcn::Bind (void)
{
  NS_LOG_FUNCTION (this);
  m_endPoint = m_udp->Allocate ();
  if (m_boundnetdevice)
    {
      m_endPoint->BindToNetDevice (m_boundnetdevice);
    }
  return FinishBind ();
}

int
UdpSocketDcqcn::Bind6 (void)
{
  NS_LOG_FUNCTION (this);
  m_endPoint6 = m_udp->Allocate6 ();
  if (m_boundnetdevice)
    {
      m_endPoint6->BindToNetDevice (m_boundnetdevice);
    }
  return FinishBind ();
}

int 
UdpSocketDcqcn::Bind (const Address &address)
{
  NS_LOG_FUNCTION (this << address);

  if (InetSocketAddress::IsMatchingType (address))
    {
      NS_ASSERT_MSG (m_endPoint == 0, "Endpoint already allocated.");

      InetSocketAddress transport = InetSocketAddress::ConvertFrom (address);
      Ipv4Address ipv4 = transport.GetIpv4 ();
      uint16_t port = transport.GetPort ();
      SetIpTos (transport.GetTos ());
      if (ipv4 == Ipv4Address::GetAny () && port == 0)
        {
          m_endPoint = m_udp->Allocate ();
        }
      else if (ipv4 == Ipv4Address::GetAny () && port != 0)
        {
          m_endPoint = m_udp->Allocate (GetBoundNetDevice (), port);
        }
      else if (ipv4 != Ipv4Address::GetAny () && port == 0)
        {
          m_endPoint = m_udp->Allocate (ipv4);
        }
      else if (ipv4 != Ipv4Address::GetAny () && port != 0)
        {
          m_endPoint = m_udp->Allocate (GetBoundNetDevice (), ipv4, port);
        }
      if (0 == m_endPoint)
        {
          m_errno = port ? ERROR_ADDRINUSE : ERROR_ADDRNOTAVAIL;
          return -1;
        }
      if (m_boundnetdevice)
        {
          m_endPoint->BindToNetDevice (m_boundnetdevice);
        }

    }
  else if (Inet6SocketAddress::IsMatchingType (address))
    {
      NS_ASSERT_MSG (m_endPoint == 0, "Endpoint already allocated.");

      Inet6SocketAddress transport = Inet6SocketAddress::ConvertFrom (address);
      Ipv6Address ipv6 = transport.GetIpv6 ();
      uint16_t port = transport.GetPort ();
      if (ipv6 == Ipv6Address::GetAny () && port == 0)
        {
          m_endPoint6 = m_udp->Allocate6 ();
        }
      else if (ipv6 == Ipv6Address::GetAny () && port != 0)
        {
          m_endPoint6 = m_udp->Allocate6 (GetBoundNetDevice (), port);
        }
      else if (ipv6 != Ipv6Address::GetAny () && port == 0)
        {
          m_endPoint6 = m_udp->Allocate6 (ipv6);
        }
      else if (ipv6 != Ipv6Address::GetAny () && port != 0)
        {
          m_endPoint6 = m_udp->Allocate6 (GetBoundNetDevice (), ipv6, port);
        }
      if (0 == m_endPoint6)
        {
          m_errno = port ? ERROR_ADDRINUSE : ERROR_ADDRNOTAVAIL;
          return -1;
        }
      if (m_boundnetdevice)
        {
          m_endPoint6->BindToNetDevice (m_boundnetdevice);
        }

      if (ipv6.IsMulticast ())
        {
          Ptr<Ipv6L3Protocol> ipv6l3 = m_node->GetObject <Ipv6L3Protocol> ();
          if (ipv6l3)
            {
              if (m_boundnetdevice == 0)
                {
                  ipv6l3->AddMulticastAddress (ipv6);
                }
              else
                {
                  uint32_t index = ipv6l3->GetInterfaceForDevice (m_boundnetdevice);
                  ipv6l3->AddMulticastAddress (m_endPoint6->GetLocalAddress (), index);
                }
            }
        }
    }
  else
    {
      NS_LOG_ERROR ("Not IsMatchingType");
      m_errno = ERROR_INVAL;
      return -1;
    }

  return FinishBind ();
}

int 
UdpSocketDcqcn::ShutdownSend (void)
{
  NS_LOG_FUNCTION (this);
  m_shutdownSend = true;
  return 0;
}

int 
UdpSocketDcqcn::ShutdownRecv (void)
{
  NS_LOG_FUNCTION (this);
  m_shutdownRecv = true;
  if (m_endPoint)
    {
      m_endPoint->SetRxEnabled (false);
    }
  if (m_endPoint6)
    {
      m_endPoint6->SetRxEnabled (false);
    }
  return 0;
}

int
UdpSocketDcqcn::Close (void)
{
  NS_LOG_FUNCTION (this);
  if (m_shutdownRecv == true && m_shutdownSend == true)
    {
      m_errno = Socket::ERROR_BADF;
      return -1;
    }
  Ipv6LeaveGroup ();
  m_shutdownRecv = true;
  m_shutdownSend = true;
  DeallocateEndPoint ();
  return 0;
}

int
UdpSocketDcqcn::Connect (const Address & address)
{
  NS_LOG_FUNCTION (this << address);
  if (InetSocketAddress::IsMatchingType(address) == true)
    {
      InetSocketAddress transport = InetSocketAddress::ConvertFrom (address);
      m_defaultAddress = Address(transport.GetIpv4 ());
      m_defaultPort = transport.GetPort ();
      SetIpTos (transport.GetTos ());
      m_connected = true;
      NotifyConnectionSucceeded ();
    }
  else if (Inet6SocketAddress::IsMatchingType(address) == true)
    {
      Inet6SocketAddress transport = Inet6SocketAddress::ConvertFrom (address);
      m_defaultAddress = Address(transport.GetIpv6 ());
      m_defaultPort = transport.GetPort ();
      m_connected = true;
      NotifyConnectionSucceeded ();
    }
  else
    {
      NotifyConnectionFailed ();
      return -1;
    }

  return 0;
}

int 
UdpSocketDcqcn::Listen (void)
{
  m_errno = Socket::ERROR_OPNOTSUPP;
  return -1;
}

int 
UdpSocketDcqcn::Send (Ptr<Packet> p, uint32_t flags) /*TODO 替换成DCQCN的版本*/
{
  NS_LOG_FUNCTION (this << p << flags);

  if (!m_connected)
    {
      m_errno = ERROR_NOTCONN;
      return -1;
    }

  return DoSend (p);
}

int 
UdpSocketDcqcn::DoSend (Ptr<Packet> p) /*TODO 替换成DCQCN的版本*/
{
  NS_LOG_FUNCTION (this << p);
  if ((m_endPoint == 0) && (Ipv4Address::IsMatchingType(m_defaultAddress) == true))
    {
      if (Bind () == -1)
        {
          NS_ASSERT (m_endPoint == 0);
          return -1;
        }
      NS_ASSERT (m_endPoint != 0);
    }
  else if ((m_endPoint6 == 0) && (Ipv6Address::IsMatchingType(m_defaultAddress) == true))
    {
      if (Bind6 () == -1)
        {
          NS_ASSERT (m_endPoint6 == 0);
          return -1;
        }
      NS_ASSERT (m_endPoint6 != 0);
    }
  if (m_shutdownSend)
    {
      m_errno = ERROR_SHUTDOWN;
      return -1;
    } 

  if (Ipv4Address::IsMatchingType (m_defaultAddress))
    {
      return DoSendTo (p, Ipv4Address::ConvertFrom (m_defaultAddress), m_defaultPort, GetIpTos ());
    }
  else if (Ipv6Address::IsMatchingType (m_defaultAddress))
    {
      return DoSendTo (p, Ipv6Address::ConvertFrom (m_defaultAddress), m_defaultPort);
    }

  m_errno = ERROR_AFNOSUPPORT;
  return(-1);
}

int
UdpSocketDcqcn::DoSendTo (Ptr<Packet> p, Ipv4Address dest, uint16_t port, uint8_t tos)
{
  NS_LOG_FUNCTION (this << p << dest << port << (uint16_t) tos);
  if (m_boundnetdevice)
    {
      NS_LOG_LOGIC ("Bound interface number " << m_boundnetdevice->GetIfIndex ());
    }
  if (m_endPoint == 0)
    {
      if (Bind () == -1)
        {
          NS_ASSERT (m_endPoint == 0);
          return -1;
        }
      NS_ASSERT (m_endPoint != 0);
    }
  if (m_shutdownSend)
    {
      m_errno = ERROR_SHUTDOWN;
      return -1;
    }

  if (p->GetSize () > GetTxAvailable () )
    {
      m_errno = ERROR_MSGSIZE;
      return -1;
    }

  uint8_t priority = GetPriority ();
  if (tos)
    {
      SocketIpTosTag ipTosTag;
      ipTosTag.SetTos (tos);
      // This packet may already have a SocketIpTosTag (see BUG 2440)
      p->ReplacePacketTag (ipTosTag);
      priority = IpTos2Priority (tos);
    }

  if (priority)
    {
      SocketPriorityTag priorityTag;
      priorityTag.SetPriority (priority);
      p->ReplacePacketTag (priorityTag);
    }

  Ptr<Ipv4> ipv4 = m_node->GetObject<Ipv4> ();

  // Locally override the IP TTL for this socket
  // We cannot directly modify the TTL at this stage, so we set a Packet tag
  // The destination can be either multicast, unicast/anycast, or
  // either all-hosts broadcast or limited (subnet-directed) broadcast.
  // For the latter two broadcast types, the TTL will later be set to one
  // irrespective of what is set in these socket options.  So, this tagging
  // may end up setting the TTL of a limited broadcast packet to be
  // the same as a unicast, but it will be fixed further down the stack
  if (m_ipMulticastTtl != 0 && dest.IsMulticast ())
    {
      SocketIpTtlTag tag;
      tag.SetTtl (m_ipMulticastTtl);
      p->AddPacketTag (tag);
    }
  else if (IsManualIpTtl () && GetIpTtl () != 0 && !dest.IsMulticast () && !dest.IsBroadcast ())
    {
      SocketIpTtlTag tag;
      tag.SetTtl (GetIpTtl ());
      p->AddPacketTag (tag);
    }
  {
    SocketSetDontFragmentTag tag;
    bool found = p->RemovePacketTag (tag);
    if (!found)
      {
        if (m_mtuDiscover)
          {
            tag.Enable ();
          }
        else
          {
            tag.Disable ();
          }
        p->AddPacketTag (tag);
      }
  }

  // Note that some systems will only send limited broadcast packets
  // out of the "default" interface; here we send it out all interfaces
  if (dest.IsBroadcast ())
    {
      if (!m_allowBroadcast)
        {
          m_errno = ERROR_OPNOTSUPP;
          return -1;
        }
      NS_LOG_LOGIC ("Limited broadcast start.");
      for (uint32_t i = 0; i < ipv4->GetNInterfaces (); i++ )
        {
          // Get the primary address
          Ipv4InterfaceAddress iaddr = ipv4->GetAddress (i, 0);
          Ipv4Address addri = iaddr.GetLocal ();
          if (addri == Ipv4Address ("127.0.0.1"))
            continue;
          // Check if interface-bound socket
          if (m_boundnetdevice) 
            {
              if (ipv4->GetNetDevice (i) != m_boundnetdevice)
                continue;
            }
          NS_LOG_LOGIC ("Sending one copy from " << addri << " to " << dest);
          m_udp->Send (p->Copy (), addri, dest,
                       m_endPoint->GetLocalPort (), port);
          NotifyDataSent (p->GetSize ());
          NotifySend (GetTxAvailable ());
        }
      NS_LOG_LOGIC ("Limited broadcast end.");
      return p->GetSize ();
    }
  else if (m_endPoint->GetLocalAddress () != Ipv4Address::GetAny ())
    {
      m_udp->Send (p->Copy (), m_endPoint->GetLocalAddress (), dest,
                   m_endPoint->GetLocalPort (), port, 0);
      NotifyDataSent (p->GetSize ());
      NotifySend (GetTxAvailable ());
      return p->GetSize ();
    }
  else if (ipv4->GetRoutingProtocol () != 0)
    {
      Ipv4Header header;
      header.SetDestination (dest);
      header.SetProtocol (UdpL4Protocol::PROT_NUMBER);
      Socket::SocketErrno errno_;
      Ptr<Ipv4Route> route;
      Ptr<NetDevice> oif = m_boundnetdevice; //specify non-zero if bound to a specific device
      // TBD-- we could cache the route and just check its validity
      route = ipv4->GetRoutingProtocol ()->RouteOutput (p, header, oif, errno_); 
      if (route != 0)
        {
          NS_LOG_LOGIC ("Route exists");
          if (!m_allowBroadcast)
            {
              // Here we try to route subnet-directed broadcasts
              uint32_t outputIfIndex = ipv4->GetInterfaceForDevice (route->GetOutputDevice ());
              uint32_t ifNAddr = ipv4->GetNAddresses (outputIfIndex);
              for (uint32_t addrI = 0; addrI < ifNAddr; ++addrI)
                {
                  Ipv4InterfaceAddress ifAddr = ipv4->GetAddress (outputIfIndex, addrI);
                  if (dest == ifAddr.GetBroadcast ())
                    {
                      m_errno = ERROR_OPNOTSUPP;
                      return -1;
                    }
                }
            }

          header.SetSource (route->GetSource ());
          m_udp->Send (p->Copy (), header.GetSource (), header.GetDestination (),
                       m_endPoint->GetLocalPort (), port, route);
          NotifyDataSent (p->GetSize ());
          return p->GetSize ();
        }
      else 
        {
          NS_LOG_LOGIC ("No route to destination");
          NS_LOG_ERROR (errno_);
          m_errno = errno_;
          return -1;
        }
    }
  else
    {
      NS_LOG_ERROR ("ERROR_NOROUTETOHOST");
      m_errno = ERROR_NOROUTETOHOST;
      return -1;
    }

  return 0;
}

int
UdpSocketDcqcn::DoSendTo (Ptr<Packet> p, Ipv6Address dest, uint16_t port) 
{
  NS_LOG_FUNCTION (this << p << dest << port);

  if (dest.IsIpv4MappedAddress ())
    {
        return (DoSendTo(p, dest.GetIpv4MappedAddress (), port, 0));
    }
  if (m_boundnetdevice)
    {
      NS_LOG_LOGIC ("Bound interface number " << m_boundnetdevice->GetIfIndex ());
    }
  if (m_endPoint6 == 0)
    {
      if (Bind6 () == -1)
        {
          NS_ASSERT (m_endPoint6 == 0);
          return -1;
        }
      NS_ASSERT (m_endPoint6 != 0);
    }
  if (m_shutdownSend)
    {
      m_errno = ERROR_SHUTDOWN;
      return -1;
    }

  if (p->GetSize () > GetTxAvailable () )
    {
      m_errno = ERROR_MSGSIZE;
      return -1;
    }

  if (IsManualIpv6Tclass ())
    {
      SocketIpv6TclassTag ipTclassTag;
      ipTclassTag.SetTclass (GetIpv6Tclass ());
      p->AddPacketTag (ipTclassTag);
    }

  uint8_t priority = GetPriority ();
  if (priority)
    {
      SocketPriorityTag priorityTag;
      priorityTag.SetPriority (priority);
      p->ReplacePacketTag (priorityTag);
    }

  Ptr<Ipv6> ipv6 = m_node->GetObject<Ipv6> ();

  // Locally override the IP TTL for this socket
  // We cannot directly modify the TTL at this stage, so we set a Packet tag
  // The destination can be either multicast, unicast/anycast, or
  // either all-hosts broadcast or limited (subnet-directed) broadcast.
  // For the latter two broadcast types, the TTL will later be set to one
  // irrespective of what is set in these socket options.  So, this tagging
  // may end up setting the TTL of a limited broadcast packet to be
  // the same as a unicast, but it will be fixed further down the stack
  if (m_ipMulticastTtl != 0 && dest.IsMulticast ())
    {
      SocketIpv6HopLimitTag tag;
      tag.SetHopLimit (m_ipMulticastTtl);
      p->AddPacketTag (tag);
    }
  else if (IsManualIpv6HopLimit () && GetIpv6HopLimit () != 0 && !dest.IsMulticast ())
    {
      SocketIpv6HopLimitTag tag;
      tag.SetHopLimit (GetIpv6HopLimit ());
      p->AddPacketTag (tag);
    }
  // There is no analgous to an IPv4 broadcast address in IPv6.
  // Instead, we use a set of link-local, site-local, and global
  // multicast addresses.  The Ipv6 routing layers should all
  // provide an interface-specific route to these addresses such
  // that we can treat these multicast addresses as "not broadcast"

  if (m_endPoint6->GetLocalAddress () != Ipv6Address::GetAny ())
    {
      m_udp->Send (p->Copy (), m_endPoint6->GetLocalAddress (), dest,
                   m_endPoint6->GetLocalPort (), port, 0);
      NotifyDataSent (p->GetSize ());
      NotifySend (GetTxAvailable ());
      return p->GetSize ();
    }
  else if (ipv6->GetRoutingProtocol () != 0)
    {
      Ipv6Header header;
      header.SetDestination (dest);
      header.SetNextHeader (UdpL4Protocol::PROT_NUMBER);
      Socket::SocketErrno errno_;
      Ptr<Ipv6Route> route;
      Ptr<NetDevice> oif = m_boundnetdevice; //specify non-zero if bound to a specific device
      // TBD-- we could cache the route and just check its validity
      route = ipv6->GetRoutingProtocol ()->RouteOutput (p, header, oif, errno_); 
      if (route != 0)
        {
          NS_LOG_LOGIC ("Route exists");
          header.SetSource (route->GetSource ());
          m_udp->Send (p->Copy (), header.GetSource (), header.GetDestination (),
                       m_endPoint6->GetLocalPort (), port, route);
          NotifyDataSent (p->GetSize ());
          return p->GetSize ();
        }
      else 
        {
          NS_LOG_LOGIC ("No route to destination");
          NS_LOG_ERROR (errno_);
          m_errno = errno_;
          return -1;
        }
    }
  else
    {
      NS_LOG_ERROR ("ERROR_NOROUTETOHOST");
      m_errno = ERROR_NOROUTETOHOST;
      return -1;
    }

  return 0;
}


// maximum message size for UDP broadcast is limited by MTU
// size of underlying link; we are not checking that now.
// \todo Check MTU size of underlying link
uint32_t
UdpSocketDcqcn::GetTxAvailable (void) const
{
  NS_LOG_FUNCTION (this);
  // No finite send buffer is modelled, but we must respect
  // the maximum size of an IP datagram (65535 bytes - headers).
  return MAX_IPV4_UDP_DATAGRAM_SIZE;
}

int 
UdpSocketDcqcn::SendTo (Ptr<Packet> p, uint32_t flags, const Address &address)
{
  NS_LOG_FUNCTION (this << p << flags << address);
  if (InetSocketAddress::IsMatchingType (address))
    {
      InetSocketAddress transport = InetSocketAddress::ConvertFrom (address);
      Ipv4Address ipv4 = transport.GetIpv4 ();
      uint16_t port = transport.GetPort ();
      uint8_t tos = transport.GetTos ();
      return DoSendTo (p, ipv4, port, tos);
    }
  else if (Inet6SocketAddress::IsMatchingType (address))
    {
      Inet6SocketAddress transport = Inet6SocketAddress::ConvertFrom (address);
      Ipv6Address ipv6 = transport.GetIpv6 ();
      uint16_t port = transport.GetPort ();
      return DoSendTo (p, ipv6, port);
    }
  return -1;
}

uint32_t
UdpSocketDcqcn::GetRxAvailable (void) const
{
  NS_LOG_FUNCTION (this);
  // We separately maintain this state to avoid walking the queue 
  // every time this might be called
  return m_rxAvailable;
}

Ptr<Packet>
UdpSocketDcqcn::Recv (uint32_t maxSize, uint32_t flags)
{
  NS_LOG_FUNCTION (this << maxSize << flags);

  Address fromAddress;
  Ptr<Packet> packet = RecvFrom (maxSize, flags, fromAddress);
  return packet;
}

Ptr<Packet>
UdpSocketDcqcn::RecvFrom (uint32_t maxSize, uint32_t flags, 
                         Address &fromAddress)
{
  NS_LOG_FUNCTION (this << maxSize << flags);

  if (m_deliveryQueue.empty () )
    {
      m_errno = ERROR_AGAIN;
      return 0;
    }
  Ptr<Packet> p = m_deliveryQueue.front ().first;
  fromAddress = m_deliveryQueue.front ().second;

  if (p->GetSize () <= maxSize)
    {
      m_deliveryQueue.pop ();
      m_rxAvailable -= p->GetSize ();
    }
  else
    {
      p = 0;
    }
  return p;
}

int
UdpSocketDcqcn::GetSockName (Address &address) const
{
  NS_LOG_FUNCTION (this << address);
  if (m_endPoint != 0)
    {
      address = InetSocketAddress (m_endPoint->GetLocalAddress (), m_endPoint->GetLocalPort ());
    }
  else if (m_endPoint6 != 0)
    {
      address = Inet6SocketAddress (m_endPoint6->GetLocalAddress (), m_endPoint6->GetLocalPort ());
    }
  else
    { // It is possible to call this method on a socket without a name
      // in which case, behavior is unspecified
      // Should this return an InetSocketAddress or an Inet6SocketAddress?
      address = InetSocketAddress (Ipv4Address::GetZero (), 0);
    }
  return 0;
}

int
UdpSocketDcqcn::GetPeerName (Address &address) const
{
  NS_LOG_FUNCTION (this << address);

  if (!m_connected)
    {
      m_errno = ERROR_NOTCONN;
      return -1;
    }

  if (Ipv4Address::IsMatchingType (m_defaultAddress))
    {
      Ipv4Address addr = Ipv4Address::ConvertFrom (m_defaultAddress);
      InetSocketAddress inet (addr, m_defaultPort);
      inet.SetTos (GetIpTos ());
      address = inet;
    }
  else if (Ipv6Address::IsMatchingType (m_defaultAddress))
    {
      Ipv6Address addr = Ipv6Address::ConvertFrom (m_defaultAddress);
      address = Inet6SocketAddress (addr, m_defaultPort);
    }
  else
    {
      NS_ASSERT_MSG (false, "unexpected address type");
    }

  return 0;
}

int 
UdpSocketDcqcn::MulticastJoinGroup (uint32_t interface, const Address &groupAddress)
{
  NS_LOG_FUNCTION (interface << groupAddress);
  /*
   1) sanity check interface
   2) sanity check that it has not been called yet on this interface/group
   3) determine address family of groupAddress
   4) locally store a list of (interface, groupAddress)
   5) call ipv4->MulticastJoinGroup () or Ipv6->MulticastJoinGroup ()
  */
  return 0;
} 

int 
UdpSocketDcqcn::MulticastLeaveGroup (uint32_t interface, const Address &groupAddress) 
{
  NS_LOG_FUNCTION (interface << groupAddress);
  /*
   1) sanity check interface
   2) determine address family of groupAddress
   3) delete from local list of (interface, groupAddress); raise a LOG_WARN
      if not already present (but return 0) 
   5) call ipv4->MulticastLeaveGroup () or Ipv6->MulticastLeaveGroup ()
  */
  return 0;
}

void
UdpSocketDcqcn::BindToNetDevice (Ptr<NetDevice> netdevice)
{
  NS_LOG_FUNCTION (netdevice);

  Ptr<NetDevice> oldBoundNetDevice = m_boundnetdevice;

  Socket::BindToNetDevice (netdevice); // Includes sanity check
  if (m_endPoint != 0)
    {
      m_endPoint->BindToNetDevice (netdevice);
    }

  if (m_endPoint6 != 0)
    {
      m_endPoint6->BindToNetDevice (netdevice);

      // The following is to fix the multicast distribution inside the node
      // and to upgrade it to the actual bound NetDevice.
      if (m_endPoint6->GetLocalAddress ().IsMulticast ())
        {
          Ptr<Ipv6L3Protocol> ipv6l3 = m_node->GetObject <Ipv6L3Protocol> ();
          if (ipv6l3)
            {
              // Cleanup old one
              if (oldBoundNetDevice)
                {
                  uint32_t index = ipv6l3->GetInterfaceForDevice (oldBoundNetDevice);
                  ipv6l3->RemoveMulticastAddress (m_endPoint6->GetLocalAddress (), index);
                }
              else
                {
                  ipv6l3->RemoveMulticastAddress (m_endPoint6->GetLocalAddress ());
                }
              // add new one
              if (netdevice)
                {
                  uint32_t index = ipv6l3->GetInterfaceForDevice (netdevice);
                  ipv6l3->AddMulticastAddress (m_endPoint6->GetLocalAddress (), index);
                }
              else
                {
                  ipv6l3->AddMulticastAddress (m_endPoint6->GetLocalAddress ());
                }
            }
        }
    }

  return;
}

void 
UdpSocketDcqcn::ForwardUp (Ptr<Packet> packet, Ipv4Header header, uint16_t port,
                          Ptr<Ipv4Interface> incomingInterface)
{
  NS_LOG_FUNCTION (this << packet << header << port);
#ifdef RDMA_RECV
  // 跳过判断坏包部分
  // if(m_receiveErrorModel && m_receiveErrorModel->IsCorrupt(packet)) {

  // }
  
  // 这里它原来是利用RemoveHeader获取的header，但是我们这里已经有header了，就用给的🤔
  uint8_t protocol = header.GetProtocol ();
  if((protocol != 0xFF && protocol != 0xFD && protocol != 0xFC) 
  // || m_node->GetNodeType() > 0
  ) {
    // This is not QCN feedback, not NACK, or I am a switch I don't care
    if(protocol != 0xFE) {  // not PFC
      // packet->AddPacketTag(FlowIdTag(m_ifIndex));
      // if(m_node->GetNodeType() == 0) {  // NIC
        // we donot have getNodeType()! so I suppose we are NIC
        if(protocol == 17) {  // look at udp only
          //! 这个其实是有的，但是unused，所以注释了，不然编译不了
          // uint16_t ecnbits = header.GetEcn();  
          UdpHeader udph;
          packet->RemoveHeader (udph);
          // SeqTsHeader sth; // we don't have SeqTsHeader
          // p->PeekHeader (sth);
          packet->AddHeader (udph);

          bool found = false;
          // uint32_t i, key = 0;

          // 我们没有m_ecn_source，摆了
          // for(i=0; i<m_ecn_source->size(); ++i) {
            // ...
          // }

          if(!found) {
            // 同上，什么都做不了
            // ...
          }

          // 下面的还是需要SeqTsHeader的，我麻了
          // ...
        }

      // 这个自然也没有😇，不过其实下面的分支好像经常用到，这个应该才是实际的发送？
      // PointToPointReceive(packet);
      } else {  // If this is a Pause, stop the corresponding queue
        // 不做PFC
        NS_ASSERT("我们不做PFC" == nullptr);
      }
    } else if(protocol == 0xFF) { // QCN on NIC
      // This is a Congestion signal
      // Then, extract data from the congestion packet.
      // We assume, without verify, the packet is destinated to me
      
      // 这里实际上是要用CnHeader的，但我们没有，就只能先这样了
      Ipv4Header ipv4h;
      packet->Copy()->RemoveHeader(ipv4h);
      // uint32_t qIndex = 
      // if(qIndex==1) return;  // DCTCP
      // uint32_t udpport = ipv4h.GetFlow();
      // uint16_t ecnbits = ipv4h.GetECNBits();
      // ... 这里一大段都是要CnHeader的，改不动
      // 涉及到m_queue, m_rate, m_rateALL, m_targetRate等

    } else if(protocol == 0xFD) { // NACK on NIC
      // qbbHeader qbbh;
      // packet->Copy()->RemoveHeader(qbbh);
      // ... 这里一大段都是要qbbHeader的，改不动
      // 涉及到m_queue, m_findex_udpport_map, m_seddingBuffer
      // , m_chunk, m_waitAck, m_waitingAck, m_nextAvail, m_retransmit
      //todo 这里涉及到了m_nextAvail，应该是个重点
    } else if(protocol == 0xFC) { // ACK on NIC
      // qbbHeader qbbh;
      // p->Copy()->RemoveHeader(qbbh);
      // ... 没qbbHeader改不动
      // 涉及到m_queue, m_findex_udpport_map, m_sendingBuffer, m_nextAvail
      // , m_ack_interval, m_backto0, m_chunk, m_waitAck, m_miletone_tx
      //todo 这里涉及到了m_nextAvail，应该是个重点
    }

#else
  if (m_shutdownRecv)
    {
      return;
    }

  // Should check via getsockopt ()..
  if (IsRecvPktInfo ())
    {
      Ipv4PacketInfoTag tag;
      packet->RemovePacketTag (tag);
      tag.SetAddress (header.GetDestination ());
      tag.SetTtl (header.GetTtl ());
      tag.SetRecvIf (incomingInterface->GetDevice ()->GetIfIndex ());
      packet->AddPacketTag (tag);
    }

  //Check only version 4 options
  if (IsIpRecvTos ())
    {
      SocketIpTosTag ipTosTag;
      ipTosTag.SetTos (header.GetTos ());
      packet->AddPacketTag (ipTosTag);
    }

  if (IsIpRecvTtl ())
    {
      SocketIpTtlTag ipTtlTag;
      ipTtlTag.SetTtl (header.GetTtl ());
      packet->AddPacketTag (ipTtlTag);
    }

  // in case the packet still has a priority tag attached, remove it
  SocketPriorityTag priorityTag;
  packet->RemovePacketTag (priorityTag);

  if ((m_rxAvailable + packet->GetSize ()) <= m_rcvBufSize)
    {
      Address address = InetSocketAddress (header.GetSource (), port);
      m_deliveryQueue.push (std::make_pair (packet, address));
      m_rxAvailable += packet->GetSize ();
      NotifyDataRecv ();
    }
  else
    {
      // In general, this case should not occur unless the
      // receiving application reads data from this socket slowly
      // in comparison to the arrival rate
      //
      // drop and trace packet
      NS_LOG_WARN ("No receive buffer space available.  Drop.");
      m_dropTrace (packet);
    }
#endif
}

void 
UdpSocketDcqcn::ForwardUp6 (Ptr<Packet> packet, Ipv6Header header, uint16_t port, Ptr<Ipv6Interface> incomingInterface)
{
  NS_LOG_FUNCTION (this << packet << header.GetSource () << port);

#ifdef RDMA_RECV

  // 由于rdma并不区分ipv4和ipv6，所以ForwardUp6基本和ForwardUp4一致

  // 跳过判断坏包部分
  // if(m_receiveErrorModel && m_receiveErrorModel->IsCorrupt(packet)) {

  // }
  
  // 这里它原来是利用RemoveHeader获取的header，但是我们这里已经有header了，就用给的🤔
  // 问题是，IPv6Header没有protocol……
  uint8_t protocol = 0;
  // uint8_t protocol = header.GetProtocol ();
  if((protocol != 0xFF && protocol != 0xFD && protocol != 0xFC) 
  // || m_node->GetNodeType() > 0
  ) {
    // This is not QCN feedback, not NACK, or I am a switch I don't care
    if(protocol != 0xFE) {  // not PFC
      // packet->AddPacketTag(FlowIdTag(m_ifIndex));
      // if(m_node->GetNodeType() == 0) {  // NIC
        // we donot have getNodeType()! so I suppose we are NIC
        if(protocol == 17) {  // look at udp only
          //! 这个其实是有的，但是unused，所以注释了，不然编译不了
          // uint16_t ecnbits = header.GetEcn();
          UdpHeader udph;
          packet->RemoveHeader (udph);
          // SeqTsHeader sth; // we don't have SeqTsHeader
          // p->PeekHeader (sth);
          packet->AddHeader (udph);

          bool found = false;
          // uint32_t i, key = 0;

          // 我们没有m_ecn_source，摆了
          // for(i=0; i<m_ecn_source->size(); ++i) {
            // ...
          // }

          if(!found) {
            // 同上，什么都做不了
            // ...
          }

          // 下面的还是需要SeqTsHeader的，我麻了
          // ...
        }

      // 这个自然也没有😇，不过其实下面的分支好像经常用到，这个应该才是实际的发送？
      // PointToPointReceive(packet);
      } else {  // If this is a Pause, stop the corresponding queue
        // 不做PFC
        NS_ASSERT("我们不做PFC" == nullptr);
      }
    } else if(protocol == 0xFF) { // QCN on NIC
      // This is a Congestion signal
      // Then, extract data from the congestion packet.
      // We assume, without verify, the packet is destinated to me
      
      // 这里实际上是要用CnHeader的，但我们没有，就只能先这样了
      Ipv6Header ipv6h;
      packet->Copy()->RemoveHeader(ipv6h);
      // uint32_t qIndex = 
      // if(qIndex==1) return;  // DCTCP
      // uint32_t udpport = ipv4h.GetFlow();
      // uint16_t ecnbits = ipv4h.GetECNBits();
      // ... 这里一大段都是要CnHeader的，改不动
      // 涉及到m_queue, m_rate, m_rateALL, m_targetRate等

    } else if(protocol == 0xFD) { // NACK on NIC
      // qbbHeader qbbh;
      // packet->Copy()->RemoveHeader(qbbh);
      // ... 这里一大段都是要qbbHeader的，改不动
      // 涉及到m_queue, m_findex_udpport_map, m_seddingBuffer
      // , m_chunk, m_waitAck, m_waitingAck, m_nextAvail, m_retransmit
      //todo 这里涉及到了m_nextAvail，应该是个重点
    } else if(protocol == 0xFC) { // ACK on NIC
      // qbbHeader qbbh;
      // p->Copy()->RemoveHeader(qbbh);
      // ... 没qbbHeader改不动
      // 涉及到m_queue, m_findex_udpport_map, m_sendingBuffer, m_nextAvail
      // , m_ack_interval, m_backto0, m_chunk, m_waitAck, m_miletone_tx
      //todo 这里涉及到了m_nextAvail，应该是个重点
    }

#else
  if (m_shutdownRecv)
    {
      return;
    }

  // Should check via getsockopt ().
  if (IsRecvPktInfo ())
    {
      Ipv6PacketInfoTag tag;
      packet->RemovePacketTag (tag);
      tag.SetAddress (header.GetDestination ());
      tag.SetHoplimit (header.GetHopLimit ());
      tag.SetTrafficClass (header.GetTrafficClass ());
      tag.SetRecvIf (incomingInterface->GetDevice ()->GetIfIndex ());
      packet->AddPacketTag (tag);
    }

  // Check only version 6 options
  if (IsIpv6RecvTclass ())
    {
      SocketIpv6TclassTag ipTclassTag;
      ipTclassTag.SetTclass (header.GetTrafficClass ());
      packet->AddPacketTag (ipTclassTag);
    }

  if (IsIpv6RecvHopLimit ())
    {
      SocketIpv6HopLimitTag ipHopLimitTag;
      ipHopLimitTag.SetHopLimit (header.GetHopLimit ());
      packet->AddPacketTag (ipHopLimitTag);
    }

  // in case the packet still has a priority tag attached, remove it
  SocketPriorityTag priorityTag;
  packet->RemovePacketTag (priorityTag);

  if ((m_rxAvailable + packet->GetSize ()) <= m_rcvBufSize)
    {
      Address address = Inet6SocketAddress (header.GetSource (), port);
      m_deliveryQueue.push (std::make_pair (packet, address));
      m_rxAvailable += packet->GetSize ();
      NotifyDataRecv ();
    }
  else
    {
      // In general, this case should not occur unless the
      // receiving application reads data from this socket slowly
      // in comparison to the arrival rate
      //
      // drop and trace packet
      NS_LOG_WARN ("No receive buffer space available.  Drop.");
      m_dropTrace (packet);
    }
#endif
}

void
UdpSocketDcqcn::ForwardIcmp (Ipv4Address icmpSource, uint8_t icmpTtl, 
                            uint8_t icmpType, uint8_t icmpCode,
                            uint32_t icmpInfo)
{
  NS_LOG_FUNCTION (this << icmpSource << (uint32_t)icmpTtl << (uint32_t)icmpType <<
                   (uint32_t)icmpCode << icmpInfo);
  if (!m_icmpCallback.IsNull ())
    {
      m_icmpCallback (icmpSource, icmpTtl, icmpType, icmpCode, icmpInfo);
    }
}

void
UdpSocketDcqcn::ForwardIcmp6 (Ipv6Address icmpSource, uint8_t icmpTtl, 
                            uint8_t icmpType, uint8_t icmpCode,
                            uint32_t icmpInfo)
{
  NS_LOG_FUNCTION (this << icmpSource << (uint32_t)icmpTtl << (uint32_t)icmpType <<
                   (uint32_t)icmpCode << icmpInfo);
  if (!m_icmpCallback6.IsNull ())
    {
      m_icmpCallback6 (icmpSource, icmpTtl, icmpType, icmpCode, icmpInfo);
    }
}

void 
UdpSocketDcqcn::SetRcvBufSize (uint32_t size)
{
  m_rcvBufSize = size;
}

uint32_t 
UdpSocketDcqcn::GetRcvBufSize (void) const
{
  return m_rcvBufSize;
}

void 
UdpSocketDcqcn::SetIpMulticastTtl (uint8_t ipTtl)
{
  m_ipMulticastTtl = ipTtl;
}

uint8_t 
UdpSocketDcqcn::GetIpMulticastTtl (void) const
{
  return m_ipMulticastTtl;
}

void 
UdpSocketDcqcn::SetIpMulticastIf (int32_t ipIf)
{
  m_ipMulticastIf = ipIf;
}

int32_t 
UdpSocketDcqcn::GetIpMulticastIf (void) const
{
  return m_ipMulticastIf;
}

void 
UdpSocketDcqcn::SetIpMulticastLoop (bool loop)
{
  m_ipMulticastLoop = loop;
}

bool 
UdpSocketDcqcn::GetIpMulticastLoop (void) const
{
  return m_ipMulticastLoop;
}

void 
UdpSocketDcqcn::SetMtuDiscover (bool discover)
{
  m_mtuDiscover = discover;
}
bool 
UdpSocketDcqcn::GetMtuDiscover (void) const
{
  return m_mtuDiscover;
}

bool
UdpSocketDcqcn::SetAllowBroadcast (bool allowBroadcast)
{
  m_allowBroadcast = allowBroadcast;
  return true;
}

bool
UdpSocketDcqcn::GetAllowBroadcast () const
{
  return m_allowBroadcast;
}

void
UdpSocketDcqcn::Ipv6JoinGroup (Ipv6Address address, Socket::Ipv6MulticastFilterMode filterMode, std::vector<Ipv6Address> sourceAddresses)
{
  NS_LOG_FUNCTION (this << address << &filterMode << &sourceAddresses);

  // We can join only one multicast group (or change its params)
  NS_ASSERT_MSG ((m_ipv6MulticastGroupAddress == address || m_ipv6MulticastGroupAddress.IsAny ()), "Can join only one IPv6 multicast group.");

  m_ipv6MulticastGroupAddress = address;

  Ptr<Ipv6L3Protocol> ipv6l3 = m_node->GetObject <Ipv6L3Protocol> ();
  if (ipv6l3)
    {
      if (filterMode == INCLUDE && sourceAddresses.empty ())
        {
          // it is a leave
          if (m_boundnetdevice)
            {
              int32_t index = ipv6l3->GetInterfaceForDevice (m_boundnetdevice);
              NS_ASSERT_MSG (index >= 0, "Interface without a valid index");
              ipv6l3->RemoveMulticastAddress (address, index);
            }
          else
            {
              ipv6l3->RemoveMulticastAddress (address);
            }
        }
      else
        {
          // it is a join or a modification
          if (m_boundnetdevice)
            {
              int32_t index = ipv6l3->GetInterfaceForDevice (m_boundnetdevice);
              NS_ASSERT_MSG (index >= 0, "Interface without a valid index");
              ipv6l3->AddMulticastAddress (address, index);
            }
          else
            {
              ipv6l3->AddMulticastAddress (address);
            }
        }
    }
}

} // namespace ns3
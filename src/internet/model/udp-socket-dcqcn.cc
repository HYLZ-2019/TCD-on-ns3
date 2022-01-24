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

#include "udp-socket-dcqcn.h"
#include "udp-l4-protocol.h"
#include "ipv4-end-point.h"
#include "ipv6-end-point.h"
#include <limits>
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/double.h"
#include "ns3/boolean.h"
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
#include "ns3/simulator.h"

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
    .AddAttribute ("bps", "Default DataRate of the Socket",
                DataRateValue (DataRate ("1Gb/s")),
                MakeDataRateAccessor (&UdpSocketDcqcn::m_bps),
                MakeDataRateChecker ())
			.AddAttribute("ClampTargetRate",
				"Clamp target rate.",
				BooleanValue(false),
				MakeBooleanAccessor(&UdpSocketDcqcn::m_EcnClampTgtRate),
				MakeBooleanChecker())
			.AddAttribute("ClampTargetRateAfterTimeInc",
				"Clamp target rate after timer increase.",
				BooleanValue(false),
				MakeBooleanAccessor(&UdpSocketDcqcn::m_EcnClampTgtRateAfterTimeInc),
				MakeBooleanChecker())
			.AddAttribute("CNPInterval",
				"The interval of generating CNP",
				DoubleValue(50.0),
				MakeDoubleAccessor(&UdpSocketDcqcn::m_qcn_interval),
				MakeDoubleChecker<double>())
			.AddAttribute("AlphaResumInterval",
				"The interval of resuming alpha",
				DoubleValue(55.0),
				MakeDoubleAccessor(&UdpSocketDcqcn::m_alpha_resume_interval),
				MakeDoubleChecker<double>())
			.AddAttribute("RPTimer",
				"The rate increase timer at RP in microseconds",
				DoubleValue(1500.0),
				MakeDoubleAccessor(&UdpSocketDcqcn::m_rpgTimeReset),
				MakeDoubleChecker<double>())
			.AddAttribute("FastRecoveryTimes",
				"The rate increase timer at RP",
				UintegerValue(5),
				MakeUintegerAccessor(&UdpSocketDcqcn::m_rpgThreshold),
				MakeUintegerChecker<uint32_t>())
			.AddAttribute("DCTCPGain",
				"Control gain parameter which determines the level of rate decrease",
				DoubleValue(1.0 / 16),
				MakeDoubleAccessor(&UdpSocketDcqcn::m_g),
				MakeDoubleChecker<double>())
			.AddAttribute("MinRate",
				"Minimum rate of a throttled flow",
				DataRateValue(DataRate("100b/s")),
				MakeDataRateAccessor(&UdpSocketDcqcn::m_minRate),
				MakeDataRateChecker())
			.AddAttribute("ByteCounter",
				"Byte counter constant for increment process.",
				UintegerValue(150000),
				MakeUintegerAccessor(&UdpSocketDcqcn::m_bc),
				MakeUintegerChecker<uint32_t>())
			.AddAttribute("RateAI",
				"Rate increment unit in AI period",
				DataRateValue(DataRate("5Mb/s")),
				MakeDataRateAccessor(&UdpSocketDcqcn::m_rai),
				MakeDataRateChecker())
			.AddAttribute("RateHAI",
				"Rate increment unit in hyperactive AI period",
				DataRateValue(DataRate("50Mb/s")),
				MakeDataRateAccessor(&UdpSocketDcqcn::m_rhai),
				MakeDataRateChecker())
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
    m_rxAvailable (0),
    m_ecnbits (-1),
	  m_qfb (0),
	  m_total (0)
{
  NS_LOG_FUNCTION (this);
  m_allowBroadcast = false;
  static uint32_t socketNum = 0;
  m_socketID = ++socketNum;

  //DCQCN Init
  m_txMachineState = READY;
  //m_credits = 0;
  for (uint32_t j = 0; j < maxHop; j++)
    {
      m_txBytes[j] = m_bc;				//we don't need this at the beginning, so it doesn't matter what value it has
      m_rpWhile[j] = m_rpgTimeReset;	//we don't need this at the beginning, so it doesn't matter what value it has
      m_rpByteStage[j] = 0;
      m_rpTimeStage[j] = 0;
      m_alpha[j] = 0.5;
      m_rpStage[j] = 0; //not in any qcn stage
    }
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

    MyTag qcnTag;
    qcnTag.SetSimpleValue (0);
    p -> AddPacketTag(qcnTag);
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
      return wrapDoSendTo (p, Ipv4Address::ConvertFrom (m_defaultAddress), m_defaultPort, GetIpTos ());
    }
  else if (Ipv6Address::IsMatchingType (m_defaultAddress)) //We don't use it;
    {
      return DoSendTo (p, Ipv6Address::ConvertFrom (m_defaultAddress), m_defaultPort);
    }

  m_errno = ERROR_AFNOSUPPORT;
  return(-1);
}

//TODO: 把DequeueAndTransmit()那一堆搬进来
//你会从m_sedingBuffer里取出需要的p, dest, port, tos，然后调用DoSendTo
void
UdpSocketDcqcn::TransmitComplete(void) {
  //std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, TransmitComplete.\n";
	NS_LOG_FUNCTION(this);
	NS_ASSERT_MSG(m_txMachineState == BUSY, "Must be BUSY if transmitting");
	m_txMachineState = READY;
	NS_ASSERT_MSG(m_currentPkt != 0, "QbbNetDevice::TransmitComplete(): m_currentPkt zero");
	m_currentPkt = 0;
	DequeueAndTransmit();
}

void 
UdpSocketDcqcn::TransmitStart(Ptr<Packet> p) {
  //std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, TransmitStart.\n";
	NS_LOG_FUNCTION(this << p);
	NS_LOG_LOGIC("UID is " << p->GetUid() << ")");
	//
	// This function is called to start the process of transmitting a packet.
	// We need to tell the channel that we've started wiggling the wire and
	// schedule an event that will be executed when the transmission is complete.
	//
	NS_ASSERT_MSG(m_txMachineState == READY, "Must be READY to transmit");
	m_txMachineState = BUSY;
	m_currentPkt = p;
	//m_phyTxBeginTrace(m_currentPkt);
	Time txTime = m_bps.CalculateBytesTxTime(p -> GetSize());
	Time txCompleteTime = txTime + m_tInterframeGap;
	NS_LOG_LOGIC("Schedule TransmitCompleteEvent in " << txCompleteTime.GetSeconds() << "sec");
	Simulator::Schedule(txCompleteTime, &UdpSocketDcqcn::TransmitComplete, this);
}


//DCQCN的速率调整部分
	void
		UdpSocketDcqcn::rpr_adjust_rates(uint32_t hop)
	{
		AdjustRates(hop, DataRate("0bps"));
		rpr_fast_recovery(hop);
		return;
	}

	void
		UdpSocketDcqcn::rpr_fast_recovery(uint32_t hop)
	{
		m_rpStage[hop] = 1;
		return;
	}


	void
		UdpSocketDcqcn::rpr_active_increase(uint32_t hop)
	{
		AdjustRates(hop, m_rai);
		m_rpStage[hop] = 2;
	}


	void
		UdpSocketDcqcn::rpr_active_byte(uint32_t hop)
	{
		m_rpByteStage[hop]++;
		m_txBytes[hop] = m_bc;
		rpr_active_increase(hop);
	}

	void
		UdpSocketDcqcn::rpr_active_time(uint32_t hop)
	{
		m_rpTimeStage[hop]++;
		m_rpWhile[hop] = m_rpgTimeReset;
		Simulator::Cancel(m_rptimer[hop]);
		m_rptimer[hop] = Simulator::Schedule(MicroSeconds(m_rpWhile[hop]), &UdpSocketDcqcn::rpr_timer_wrapper, this, hop);
		rpr_active_select(hop);
	}


	void
		UdpSocketDcqcn::rpr_fast_byte(uint32_t hop)
	{
		m_rpByteStage[hop]++;
		m_txBytes[hop] = m_bc;
		if (m_rpByteStage[hop] < m_rpgThreshold)
		{
			rpr_adjust_rates(hop);
		}
		else
		{
			rpr_active_select(hop);
		}
			
		return;
	}

	void
		UdpSocketDcqcn::rpr_fast_time(uint32_t hop)
	{
		m_rpTimeStage[hop]++;
		m_rpWhile[hop] = m_rpgTimeReset;
		Simulator::Cancel(m_rptimer[hop]);
		m_rptimer[hop] = Simulator::Schedule(MicroSeconds(m_rpWhile[hop]), &UdpSocketDcqcn::rpr_timer_wrapper, this, hop);
		if (m_rpTimeStage[hop] < m_rpgThreshold)
			rpr_adjust_rates(hop);
		else
			rpr_active_select(hop);
		return;
	}


	void
		UdpSocketDcqcn::rpr_hyper_byte(uint32_t hop)
	{
		m_rpByteStage[hop]++;
		m_txBytes[hop] = m_bc / 2;
		rpr_hyper_increase(hop);
	}


	void
		UdpSocketDcqcn::rpr_hyper_time(uint32_t hop)
	{
		m_rpTimeStage[hop]++;
		m_rpWhile[hop] = m_rpgTimeReset / 2;
		Simulator::Cancel(m_rptimer[hop]);
		m_rptimer[hop] = Simulator::Schedule(MicroSeconds(m_rpWhile[hop]), &UdpSocketDcqcn::rpr_timer_wrapper, this, hop);
		rpr_hyper_increase(hop);
	}


	void
		UdpSocketDcqcn::rpr_active_select(uint32_t hop)
	{
		if (m_rpByteStage[hop] < m_rpgThreshold || m_rpTimeStage[hop] < m_rpgThreshold)
			rpr_active_increase(hop);
		else
			rpr_hyper_increase(hop);
		return;
	}


	void
		UdpSocketDcqcn::rpr_hyper_increase(uint32_t hop)
	{
		AdjustRates(hop, m_rhai*(double)(std::min(m_rpByteStage[hop], m_rpTimeStage[hop]) - m_rpgThreshold + 1));
		m_rpStage[hop] = 3;
		return;
	}

	void
		UdpSocketDcqcn::AdjustRates(uint32_t hop, DataRate increase)
	{
		if (((m_rpByteStage[hop] == 1) || (m_rpTimeStage[hop] == 1)) && (m_targetRate[hop] > m_rateAll[hop] * (uint64_t)10))
			m_targetRate[hop] /= 8;
		else
			m_targetRate[hop] += increase;

		m_rateAll[hop] = (m_rateAll[hop] / 2) + (m_targetRate[hop] / 2);

		if (m_rateAll[hop] > m_bps)
			m_rateAll[hop] = m_bps;

		m_rate = m_bps;
		for (uint32_t j = 0; j < maxHop; j++)
			m_rate = std::min(m_rate, m_rateAll[j]);
		return;
	}

	void
		UdpSocketDcqcn::rpr_cnm_received(uint32_t hop, double fraction)
	{
		if (!m_EcnClampTgtRateAfterTimeInc && !m_EcnClampTgtRate)
		{
			if (m_rpByteStage[hop] != 0)
			{
				m_targetRate[hop] = m_rateAll[hop];
				m_txBytes[hop] = m_bc;
			}
		}
		else if (m_EcnClampTgtRate)
		{
			m_targetRate[hop] = m_rateAll[hop];
			m_txBytes[hop] = m_bc; //for fluid model, QCN standard doesn't have this.
		}
		else
		{
			if (m_rpByteStage[hop] != 0 || m_rpTimeStage[hop] != 0)
			{
				m_targetRate[hop] = m_rateAll[hop];
				m_txBytes[hop] = m_bc;
			}
		}

    std::cout << "call rpr_cnm_received with m_alpha_resume_interval="<<m_alpha_resume_interval<<std::endl;
		m_rpByteStage[hop] = 0;
		m_rpTimeStage[hop] = 0;
		//m_alpha[findex][hop] = (1-m_g)*m_alpha[findex][hop] + m_g*fraction;
		m_alpha[hop] = (1 - m_g)*m_alpha[hop] + m_g; 	//binary feedback
		m_rateAll[hop] = std::max(m_minRate, m_rateAll[hop] * (1 - m_alpha[hop] / 2));
		Simulator::Cancel(m_resumeAlpha[hop]);
		m_resumeAlpha[hop] = Simulator::Schedule(MicroSeconds(m_alpha_resume_interval), &UdpSocketDcqcn::ResumeAlpha, this, hop);
		m_rpWhile[hop] = m_rpgTimeReset;
		Simulator::Cancel(m_rptimer[hop]);
		m_rptimer[hop] = Simulator::Schedule(MicroSeconds(m_rpWhile[hop]), &UdpSocketDcqcn::rpr_timer_wrapper, this, hop);
		rpr_fast_recovery(hop);
	}

	void
		UdpSocketDcqcn::rpr_timer_wrapper(uint32_t hop)
	{
		if (m_rpStage[hop] == 1)
		{
			rpr_fast_time(hop);
		}
		else if (m_rpStage[hop] == 2)
		{
			rpr_active_time(hop);
		}
		else if (m_rpStage[hop] == 3)
		{
			rpr_hyper_time(hop);
		}
		return;
	}

	void
		UdpSocketDcqcn::ResumeAlpha(uint32_t hop)
	{
		m_alpha[hop] = (1 - m_g)*m_alpha[hop];
		Simulator::Cancel(m_resumeAlpha[hop]);
		m_resumeAlpha[hop] = Simulator::Schedule(MicroSeconds(m_alpha_resume_interval), &UdpSocketDcqcn::ResumeAlpha, this, hop);
	}
//DCQCN的速率调整部分
void
UdpSocketDcqcn::DequeueAndTransmit(void) {
	NS_LOG_FUNCTION(this);

  //std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, DequeueAndTransmit.\n";
	if (m_txMachineState == BUSY) return;	// Quit if channel busy

	//没有要发的包了，return
	if (m_sendingBuffer.empty()) return;
	
	if (m_nextAvail <= Simulator::GetMaximumSimulationTime()) { //立刻发包
		BufferItem item = m_sendingBuffer.front();
    m_sendingBuffer.pop();
		if (m_rate == 0) {			//late initialization	
			m_rate = m_bps;
			for (uint32_t j = 0; j < maxHop; j++) {
				m_rateAll[j] = m_bps, m_targetRate[j] = m_bps;
			}
		}
    //std::cout << "m_bps:"<<m_bps<<"\n";
		//double creditsDue = std::max(0.0, (double)m_bps.GetBitRate() / m_rate.GetBitRate() * (item.p->GetSize() - m_credits));
		Time nextSend = m_tInterframeGap + m_rate.CalculateBytesTxTime(item.p->GetSize());
		m_nextAvail = Simulator::Now() + nextSend;

		//m_credits = 0;	//reset credits
		for (uint32_t i = 0; i < 1; i++)
		{
			if (m_rpStage[i] > 0) m_txBytes[i] -= item.p->GetSize();
			else m_txBytes[i] = m_bc;
			if (m_txBytes[i] < 0) {
				if (m_rpStage[i] == 1) {
					rpr_fast_byte(i);
				}
				else if (m_rpStage[i] == 2) {
					rpr_active_byte(i);
				}
				else if (m_rpStage[i] == 3) {
					rpr_hyper_byte(i);
				}
			}
		}
		TransmitStart(item.p);

    //std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, Call DoSendTo(), m_nextAvail="<< m_nextAvail.GetSeconds()<<".\n";
		DoSendTo(item.p, item.dest, item.port, item.tos);
		return;
	}
	
  Time t = Simulator::GetMaximumSimulationTime();
	//不能立刻发的预约一下
	if (m_nextSend.IsExpired() &&
		m_nextAvail < Simulator::GetMaximumSimulationTime() &&
		m_nextAvail.GetTimeStep() > Simulator::Now().GetTimeStep()) {
		NS_LOG_LOGIC("Next DequeueAndTransmit at " << t << " or " << (t - Simulator::Now()) << " later");
		NS_ASSERT(t > Simulator::Now());
		m_nextSend = Simulator::Schedule(t - Simulator::Now(), &UdpSocketDcqcn::DequeueAndTransmit, this);
	}

	return;
}

int UdpSocketDcqcn::wrapDoSendTo(Ptr<Packet> p, Ipv4Address dest, uint16_t port, uint8_t tos) {

  std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, wrapDoSendTo: packet=[" << p << "], dest=[" << dest << "], port=[" << port << "].\n";
	m_sendingBuffer.push(BufferItem(p, dest, port, tos)); //TODO:定义一下类型
	
	DequeueAndTransmit();
  return 0;
}

int
UdpSocketDcqcn::DoSendTo (Ptr<Packet> p, Ipv4Address dest, uint16_t port, uint8_t tos)
{
  static int num = 0; ++num;
  std::cout << "*************the [" << num <<"]th of ipv4DoSendTo() with tos is called\n";
  std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, DoSendTo: packet=[" << p << "], dest=[" << dest << "], port=[" << port << "].\n";
 
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
UdpSocketDcqcn::DoSendTo (Ptr<Packet> p, Ipv4Address dest, uint16_t port)
{
  std::cout << "DoSendTo Ipv4 without tos\n";
  std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, DoSendTo: packet=[" << p << "], dest=[" << dest << "], port=[" << port << "].\n";
  NS_LOG_FUNCTION (this << p << dest << port);
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
  //
  // If dest is set to the limited broadcast address (all ones),
  // convert it to send a copy of the packet out of every 
  // interface as a subnet-directed broadcast.
  // Exception:  if the interface has a /32 address, there is no
  // valid subnet-directed broadcast, so send it as limited broadcast
  // Note also that some systems will only send limited broadcast packets
  // out of the "default" interface; here we send it out all interfaces
  //
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
          Ipv4Mask maski = iaddr.GetMask ();
          if (maski == Ipv4Mask::GetOnes ())
            {
              // if the network mask is 255.255.255.255, do not convert dest
              NS_LOG_LOGIC ("Sending one copy from " << addri << " to " << dest
                                                     << " (mask is " << maski << ")");
              m_udp->Send (p->Copy (), addri, dest,
                           m_endPoint->GetLocalPort (), port);
              NotifyDataSent (p->GetSize ());
              NotifySend (GetTxAvailable ());
            }
          else
            {
              // Convert to subnet-directed broadcast
              Ipv4Address bcast = addri.GetSubnetDirectedBroadcast (maski);
              NS_LOG_LOGIC ("Sending one copy from " << addri << " to " << bcast
                                                     << " (mask is " << maski << ")");
              m_udp->Send (p->Copy (), addri, bcast,
                           m_endPoint->GetLocalPort (), port);
              NotifyDataSent (p->GetSize ());
              NotifySend (GetTxAvailable ());
            }
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
  std::cout << "DoSendTo Ipv6 without tos\n";
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
      return wrapDoSendTo (p, ipv4, port, tos);
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
UdpSocketDcqcn::CheckandSendQCN(Ipv4Address source, uint32_t port) {
  if (m_total != 0) {  
    std::cout << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
    std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, CheckandSendQCN() is called. The socketID is "<< m_socketID<<"\n";
    std::cout << "m_ecnbits = [" << (int)m_ecnbits << "], m_qfb = [" << m_qfb << "], m_total = [" << m_total << "].\n";
    std::cout << "--------------------------------------------------------------------------------------------\n";
  }
  bool iscongested = (m_ecnbits & (TCD_CONGESTED_BIT | TCD_UNDETERMINED_BIT));
  if (iscongested) {
			//构造一个QCN包发出去, 发这个QCN包不受Traffic Control限制
			Ptr<Packet> p = new Packet(0); 
			MyTag qcnTag;
			qcnTag.SetSimpleValue (((uint64_t)m_total << 48) | ((uint64_t)m_qfb << 32) | ((uint64_t)m_ecnbits << 16) | TCD_QCN_BIT);
			p -> AddPacketTag(qcnTag);
			//参数需要编
			DoSendTo (p, source, port, GetIpTos ());
      std::cout << qcnTag.GetSimpleValue() << std::endl;
			//DoSendTo就直接发包了 m_udp -> Send (p->Copy (), addri, dest, m_endPoint->GetLocalPort (), port);
	}
  m_ecnbits = 0;
  m_qfb = m_total = 0;
  Simulator::Schedule(MicroSeconds(m_qcn_interval), &UdpSocketDcqcn::CheckandSendQCN, this, source, port);
}

void 
UdpSocketDcqcn::ForwardUp (Ptr<Packet> packet, Ipv4Header header, uint16_t port,
                          Ptr<Ipv4Interface> incomingInterface)
{
  std::cout << "----------------------------------------------------------------------\n";
  static int num = 0; ++num;
  std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, the {" << num << "}th of UdpSocketDcqcn::ForwardUp; The socketID is "<< m_socketID<<"\n";
  std::cout << "packet=[" << packet << "], header=[" << header << "], port=[" << port << "].\n";
 
	NS_LOG_FUNCTION (this << packet << header << port);
	if (m_shutdownRecv) {
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
	
	//uint8_t protocol = header.GetProtocol ();
	//protocol值17是UDP, 发包的时候，经过m_upd.send(UDPL4Protocol::send), protocol号全部是17
	
	//准备用MyTag类型
	//simple value值为1, 2, 3的情况对应TCD三元组 其中3是congestion
	//QCN对应simple value的值是4的情况
	MyTag myTag;
	packet -> RemovePacketTag(myTag);
	uint64_t simpleValue = myTag.GetSimpleValue();
	
  std::cout << "with Tag ["<< (uint)simpleValue << "] on the packet.\n";

  std::cout << "----------------------------------------------------------------------\n";
	bool isQCN = ((simpleValue & TCD_QCN_BIT) != 0);
	if(!isQCN) { //如果是数据包
		//收包
		Address address = InetSocketAddress (header.GetSource (), port);
		SocketAddressTag tag;
		tag.SetAddress (address);
		packet->AddPacketTag (tag);
		m_deliveryQueue.push (std::make_pair (packet, address));
		m_rxAvailable += packet->GetSize ();
		NotifyDataRecv ();
		
		//检查packet,如果有拥塞标记就往回发QCN,标记怎么打还没确定,要和TCLayer一致
		//对应qddnetdevice里的CheckandSendQCN()
    
    uint8_t myecnbits = (simpleValue & TCD_ECN_MASK);
		// The 3 bits represent whether a packet has gone through CONG/UNDET/NONCON queues.
    std::cout << (int)((int8_t)m_ecnbits) << std::endl;
    if ((int8_t)m_ecnbits == -1) {
      m_ecnbits = myecnbits;
      m_qfb = (myecnbits != 0 ? 1 : 0);
      m_total = 1;
			Ipv4Address ipv4 = header.GetSource();
      CheckandSendQCN(ipv4, port);
    }
		else {
      m_ecnbits |= myecnbits;
      m_qfb += (myecnbits != 0 ? 1 : 0);
      m_total++;
    }
	}
	else { //如果是QCN
      // This is a Congestion signal
      // Then, extract data from the congestion packet.
      // We assume, without verify, the packet is destinated to me
		
		//从包里抽出m_total & m_qfb
    uint8_t ecnbits = ((simpleValue >> 16) & TCD_ECN_MASK);
    uint16_t qfb = ((simpleValue >> 32) & MASK);
    uint16_t total = ((simpleValue >> 48) & MASK);
    std::cout << "ecnbits="<< (int)ecnbits << ", qfb=" << qfb << ", total="<<total << std::endl;
		if (m_rate == 0) { //lazy initialization	
			m_rate = m_bps;
			for (uint32_t j = 0; j < maxHop; j++) {
				m_rateAll[j] = m_bps;
				m_targetRate[j] = m_bps;	//targetrate remembers the last rate
			}
		}
		
    std::cout << "(A)M_Rate="<<m_rate<<std::endl;
		if (ecnbits & TCD_CONGESTED_BIT) { //这应该是QCN的一部分
			rpr_cnm_received(0, qfb*1.0 / (total + 1)); //这是DCQCN的一部分，DCQCN的部分都要搬进来
		}

		m_rate = m_bps;
		for (uint32_t j = 0; j < maxHop; j++)
			m_rate = std::min(m_rate, m_rateAll[j]);
    std::cout << "(B)M_Rate="<<m_rate<<std::endl;
    } 
}

void 
UdpSocketDcqcn::ForwardUp6 (Ptr<Packet> packet, Ipv6Header header, uint16_t port, Ptr<Ipv6Interface> incomingInterface)
{
  NS_LOG_FUNCTION (this << packet << header.GetSource () << port);
  std::cout << "[Error]: ForwardUp6() is called\n";
  return;
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

TypeId 
MyTag::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MyTag")
    .SetParent<Tag> ()
    .AddConstructor<MyTag> ()
    .AddAttribute ("SimpleValue",
                   "A simple value",
                   EmptyAttributeValue (),
                   MakeUintegerAccessor (&MyTag::GetSimpleValue),
                   MakeUintegerChecker<uint64_t> ())
  ;
  return tid;
}
TypeId 
MyTag::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}
uint32_t 
MyTag::GetSerializedSize (void) const
{
  return 8;
}
void 
MyTag::Serialize (TagBuffer i) const
{
  i.WriteU64 (m_simpleValue);
}
void 
MyTag::Deserialize (TagBuffer i)
{
  m_simpleValue = i.ReadU64 ();
}
void 
MyTag::Print (std::ostream &os) const
{
  os << "v=" << (uint32_t)m_simpleValue << "[TCD_CONGESTED="<<!!(m_simpleValue & TCD_CONGESTED_BIT) << ", TCD_UNDETERMINED="<<!!(m_simpleValue & TCD_UNDETERMINED_BIT)<<", TCD_NONCONGESTED="<<!!(m_simpleValue & TCD_NONCONGESTED_BIT)<<"]";
}
void 
MyTag::SetSimpleValue (uint64_t value)
{
  m_simpleValue = value;
}
uint64_t 
MyTag::GetSimpleValue (void) const
{
  return m_simpleValue;
}

SocketAddressTag::SocketAddressTag ()
{
  NS_LOG_FUNCTION (this);
}

void
SocketAddressTag::SetAddress (Address addr)
{
  NS_LOG_FUNCTION (this << addr);
  m_address = addr;
}

Address
SocketAddressTag::GetAddress (void) const
{
  NS_LOG_FUNCTION (this);
  return m_address;
}

NS_OBJECT_ENSURE_REGISTERED (SocketAddressTag);

TypeId
SocketAddressTag::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SocketAddressTag")
    .SetParent<Tag> ()
    .AddConstructor<SocketAddressTag> ()
  ;
  return tid;
}
TypeId
SocketAddressTag::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}
uint32_t
SocketAddressTag::GetSerializedSize (void) const
{
  NS_LOG_FUNCTION (this);
  return m_address.GetSerializedSize ();
}
void
SocketAddressTag::Serialize (TagBuffer i) const
{
  NS_LOG_FUNCTION (this << &i);
  m_address.Serialize (i);
}
void
SocketAddressTag::Deserialize (TagBuffer i)
{
  NS_LOG_FUNCTION (this << &i);
  m_address.Deserialize (i);
}
void
SocketAddressTag::Print (std::ostream &os) const
{
  NS_LOG_FUNCTION (this << &os);
  os << "address=" << m_address;
}


} // namespace ns3

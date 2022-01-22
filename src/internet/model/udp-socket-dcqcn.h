//DCQCN socket type, 我们在这里实现拥塞控制，然后用它来替换impl
#ifndef UDP_SOCKET_DCQCN_H
#define UDP_SOCKET_DCQCN_H

#include <stdint.h>
#include <queue>
#include "ns3/callback.h"
#include "ns3/traced-callback.h"
#include "ns3/socket.h"
#include "ns3/ptr.h"
#include "ns3/ipv4-address.h"
#include "ns3/udp-socket.h"
#include "ns3/ipv4-interface.h"
#include "ns3/data-rate.h"
#include "ns3/tag.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "icmpv4.h" //这个是原来ICMP包的报头，可以考虑复用或者替换

namespace ns3 {

class Ipv4EndPoint;
class Ipv6EndPoint;
class Node;
class Packet;
class UdpL4Protocol;
class Ipv6Header;
class Ipv6Interface;

/**
 * \ingroup socket
 * \ingroup udp
 *
 * \brief A sockets interface to UDP
 * 
 * This class subclasses ns3::UdpSocket, and provides a socket interface
 * to ns3's implementation of UDP.
 */
//这个DCQCN的原型是IMPL,我们需要做的是把send, receive替换成带有拥塞控制的版本，如果有新的数据结构就放到这个类里
class BufferItem{ //定义Item类型方便管理
public:
	Ptr<Packet> p;
	Ipv4Address dest;
	uint16_t port; 
	uint8_t tos;
	BufferItem(Ptr<Packet> _p, Ipv4Address _dest, uint16_t _port, uint8_t _tos) {
		p = _p, dest = _dest, port = _port, tos = _tos;
	}
};


class UdpSocketDcqcn : public UdpSocket
{
public:
  uint32_t m_socketID;
  static const uint32_t maxHop = 1; // Max hop count in the network. should not exceed 16 
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);
  /**
   * Create an unbound udp socket.
   */
  UdpSocketDcqcn ();
  virtual ~UdpSocketDcqcn ();

  /**
   * \brief Set the associated node.
   * \param node the node
   */
  void SetNode (Ptr<Node> node);
  /**
   * \brief Set the associated UDP L4 protocol.
   * \param udp the UDP L4 protocol
   */
  void SetUdp (Ptr<UdpL4Protocol> udp);

  virtual enum SocketErrno GetErrno (void) const;
  virtual enum SocketType GetSocketType (void) const;
  virtual Ptr<Node> GetNode (void) const;
  virtual int Bind (void);
  virtual int Bind6 (void);
  virtual int Bind (const Address &address);
  virtual int Close (void);
  virtual int ShutdownSend (void);
  virtual int ShutdownRecv (void);
  virtual int Connect (const Address &address);
  virtual int Listen (void);
  virtual uint32_t GetTxAvailable (void) const;
  virtual int Send (Ptr<Packet> p, uint32_t flags); //TODO
  virtual int SendTo (Ptr<Packet> p, uint32_t flags, const Address &address); //TODO
  virtual uint32_t GetRxAvailable (void) const;
  virtual Ptr<Packet> Recv (uint32_t maxSize, uint32_t flags); //TODO
  virtual Ptr<Packet> RecvFrom (uint32_t maxSize, uint32_t flags, //TODO
                                Address &fromAddress);
  virtual int GetSockName (Address &address) const; 
  virtual int GetPeerName (Address &address) const;
  virtual int MulticastJoinGroup (uint32_t interfaceIndex, const Address &groupAddress);
  virtual int MulticastLeaveGroup (uint32_t interfaceIndex, const Address &groupAddress);
  virtual void BindToNetDevice (Ptr<NetDevice> netdevice);
  virtual bool SetAllowBroadcast (bool allowBroadcast);
  virtual bool GetAllowBroadcast () const;
  virtual void Ipv6JoinGroup (Ipv6Address address, Socket::Ipv6MulticastFilterMode filterMode, std::vector<Ipv6Address> sourceAddresses);

private:
  // Attributes set through UdpSocket base class 
  virtual void SetRcvBufSize (uint32_t size);
  virtual uint32_t GetRcvBufSize (void) const;
  virtual void SetIpMulticastTtl (uint8_t ipTtl);
  virtual uint8_t GetIpMulticastTtl (void) const;
  virtual void SetIpMulticastIf (int32_t ipIf);
  virtual int32_t GetIpMulticastIf (void) const;
  virtual void SetIpMulticastLoop (bool loop);
  virtual bool GetIpMulticastLoop (void) const;
  virtual void SetMtuDiscover (bool discover);
  virtual bool GetMtuDiscover (void) const;


  /**
   * \brief UdpSocketFactory friend class.
   * \relates UdpSocketFactory
   */
  friend class UdpSocketFactory;
  // invoked by Udp class

  /**
   * Finish the binding process
   * \returns 0 on success, -1 on failure
   */
  int FinishBind (void);

  void DequeueAndTransmit(void);
  void TransmitStart(Ptr<Packet> p);
  void TransmitComplete(void);

  /**
   * \brief Called by the L3 protocol when it received a packet to pass on to TCP.
   *
   * \param packet the incoming packet
   * \param header the packet's IPv4 header
   * \param port the remote port
   * \param incomingInterface the incoming interface
   */
  void ForwardUp (Ptr<Packet> packet, Ipv4Header header, uint16_t port, Ptr<Ipv4Interface> incomingInterface);

  /**
   * \brief Called by the L3 protocol when it received a packet to pass on to TCP.
   *
   * \param packet the incoming packet
   * \param header the packet's IPv6 header
   * \param port the remote port
   * \param incomingInterface the incoming interface
   */
  void ForwardUp6 (Ptr<Packet> packet, Ipv6Header header, uint16_t port, Ptr<Ipv6Interface> incomingInterface);

  /**
   * \brief Kill this socket by zeroing its attributes (IPv4)
   *
   * This is a callback function configured to m_endpoint in
   * SetupCallback(), invoked when the endpoint is destroyed.
   */
  void Destroy (void);

  /**
   * \brief Kill this socket by zeroing its attributes (IPv6)
   *
   * This is a callback function configured to m_endpoint in
   * SetupCallback(), invoked when the endpoint is destroyed.
   */
  void Destroy6 (void);

  /**
   * \brief Deallocate m_endPoint and m_endPoint6
   */
  void DeallocateEndPoint (void);

  /**
   * \brief Send a packet
   * \param p packet
   * \returns 0 on success, -1 on failure
   */
  int DoSend (Ptr<Packet> p);
  /**
   * \brief Send a packet to a specific destination and port (IPv4)
   * \param p packet
   * \param daddr destination address
   * \param dport destination port
   * \param tos ToS
   * \returns 0 on success, -1 on failure
   */
  int DoSendTo (Ptr<Packet> p, Ipv4Address daddr, uint16_t dport, uint8_t tos);
  int DoSendTo (Ptr<Packet> p, Ipv4Address daddr, uint16_t dport);
  int wrapDoSendTo(Ptr<Packet> p, Ipv4Address dest, uint16_t port, uint8_t tos);  // why wrap?
  /**
   * \brief Send a packet to a specific destination and port (IPv6)
   * \param p packet
   * \param daddr destination address
   * \param dport destination port
   * \returns 0 on success, -1 on failure
   */
  int DoSendTo (Ptr<Packet> p, Ipv6Address daddr, uint16_t dport);
  //int wrapDoSendTo(Ptr<Packet> p, Ipv6Address dest, uint16_t port); 

  /**
   * \brief Called by the L3 protocol when it received an ICMP packet to pass on to TCP.
   *
   * \param icmpSource the ICMP source address
   * \param icmpTtl the ICMP Time to Live
   * \param icmpType the ICMP Type
   * \param icmpCode the ICMP Code
   * \param icmpInfo the ICMP Info
   */
  void ForwardIcmp (Ipv4Address icmpSource, uint8_t icmpTtl, uint8_t icmpType, uint8_t icmpCode, uint32_t icmpInfo);

  /**
   * \brief Called by the L3 protocol when it received an ICMPv6 packet to pass on to TCP.
   *
   * \param icmpSource the ICMP source address
   * \param icmpTtl the ICMP Time to Live
   * \param icmpType the ICMP Type
   * \param icmpCode the ICMP Code
   * \param icmpInfo the ICMP Info
   */
  void ForwardIcmp6 (Ipv6Address icmpSource, uint8_t icmpTtl, uint8_t icmpType, uint8_t icmpCode, uint32_t icmpInfo);

   /**
   * Enumeration of the states of the transmit machine of the net device.
   */
  enum TxMachineState
  {
    READY,   /**< The transmitter is ready to begin transmission of a packet */
    BUSY,    /**< The transmitter is busy transmitting a packet */
    GAP,      /**< The transmitter is in the interframe gap time */
    BACKOFF      /**< The transmitter is waiting for the channel to be free */
  };

  /**
   * The state of the Net Device transmit state machine.
   * \see TxMachineState
   */
  TxMachineState m_txMachineState;
  Ptr<Packet> m_currentPkt;
  DataRate    m_bps;
  Time m_tInterframeGap;
  double m_rpgTimeReset;
  EventId m_rptimer[maxHop];  // 去掉了fcnt
  uint32_t m_rpgThreshold;
  DataRate m_rai;		//< Rate of additive increase
  DataRate m_rhai;		//< Rate of hyper-additive increase
  DataRate m_minRate;		//< Min sending rate
  uint32_t m_bc;
  bool m_EcnClampTgtRateAfterTimeInc;
  bool m_EcnClampTgtRate;
  double m_alpha[maxHop];
  double m_alpha_resume_interval;
  double m_g;
  double m_qcn_interval;
  EventId  m_nextSend;		//< The next send event

  // Connections to other layers of TCP/IP
  Ipv4EndPoint*       m_endPoint;   //!< the IPv4 endpoint
  Ipv6EndPoint*       m_endPoint6;  //!< the IPv6 endpoint
  Ptr<Node>           m_node;       //!< the associated node
  Ptr<UdpL4Protocol> m_udp;         //!< the associated UDP L4 protocol
  Callback<void, Ipv4Address,uint8_t,uint8_t,uint8_t,uint32_t> m_icmpCallback;  //!< ICMP callback
  Callback<void, Ipv6Address,uint8_t,uint8_t,uint8_t,uint32_t> m_icmpCallback6; //!< ICMPv6 callback

  Address m_defaultAddress; //!< Default address
  uint16_t m_defaultPort;   //!< Default port
  TracedCallback<Ptr<const Packet> > m_dropTrace; //!< Trace for dropped packets

  mutable enum SocketErrno m_errno;           //!< Socket error code
  bool                     m_shutdownSend;    //!< Send no longer allowed
  bool                     m_shutdownRecv;    //!< Receive no longer allowed
  bool                     m_connected;       //!< Connection established
  bool                     m_allowBroadcast;  //!< Allow send broadcast packets

  std::queue<std::pair<Ptr<Packet>, Address> > m_deliveryQueue; //!< Queue for incoming packets
  uint32_t m_rxAvailable;                   //!< Number of available bytes to be received

  // Socket attributes
  uint32_t m_rcvBufSize;    //!< Receive buffer size
  uint8_t m_ipMulticastTtl; //!< Multicast TTL
  int32_t m_ipMulticastIf;  //!< Multicast Interface
  bool m_ipMulticastLoop;   //!< Allow multicast loop
  bool m_mtuDiscover;       //!< Allow MTU discovery
  
  //加入新组件sendingBuffer
  std :: queue <BufferItem> m_sendingBuffer;
  
  EventId m_resumeAlpha[maxHop];
  DataRate m_rateAll[maxHop];

  DataRate m_targetRate[maxHop];	//< Target rate
  DataRate m_rate;	//< Current rate
  int64_t  m_txBytes[maxHop];	//< Tx byte counter
  double  m_rpWhile[maxHop];	//< Tx byte counter
  uint32_t m_rpByteStage[maxHop];	//< Count of Tx-based rate increments
  uint32_t m_rpTimeStage[maxHop];
  uint32_t m_rpStage[maxHop]; //1: fr; 2: ai; 3: hi
  Time     m_nextAvail;	//< Soonest time of next send
  double   m_credits;	//< Credits accumulated
  EventId m_rateIncrease; // rate increase event (QCN)
  uint8_t m_ecnbits;
	uint16_t m_qfb;
	uint16_t m_total;
  
  
  void ResumeAlpha(uint32_t hop);
  void AdjustRates(uint32_t hop, DataRate increase);
  void rpr_adjust_rates(uint32_t hop);
  void rpr_fast_recovery(uint32_t hop);
  void rpr_active_increase(uint32_t hop);
  void rpr_active_byte(uint32_t hop);
  void rpr_active_time(uint32_t hop);
  void rpr_fast_byte(uint32_t hop);
  void rpr_fast_time(uint32_t hop);
  void rpr_hyper_byte(uint32_t hop);
  void rpr_hyper_time(uint32_t hop);
  void rpr_active_select(uint32_t hop);
  void rpr_hyper_increase(uint32_t hop);
  void rpr_cnm_received(uint32_t hop, double fraction);
  void rpr_timer_wrapper(uint32_t hop);
  void CheckandSendQCN(Ipv4Address source, uint32_t port);
};

class MyTag : public Tag
{
public:
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual uint32_t GetSerializedSize (void) const;
  virtual void Serialize (TagBuffer i) const;
  virtual void Deserialize (TagBuffer i);
  virtual void Print (std::ostream &os) const;

  // these are our accessors to our tag structure
  /**
   * Set the tag value
   * \param value The tag value.
   */
  void SetSimpleValue (uint8_t value);
  /**
   * Get the tag value
   * \return the tag value.
   */
  uint8_t GetSimpleValue (void) const;
private:
  uint8_t m_simpleValue;  //!< tag value
};

class SocketAddressTag : public Tag
{
public:
  SocketAddressTag ();
  void SetAddress (Address addr);
  Address GetAddress (void) const;

  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual uint32_t GetSerializedSize (void) const;
  virtual void Serialize (TagBuffer i) const;
  virtual void Deserialize (TagBuffer i);
  virtual void Print (std::ostream &os) const;

private:
  Address m_address;
};

} // namespace ns3

#endif /* UDP_SOCKET_DCQCN_H */

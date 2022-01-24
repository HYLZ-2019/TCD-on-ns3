/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2017 Universita' degli Studi di Napoli Federico II
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
 * Authors:  Stefano Avallone <stavallo@unina.it>
 */

#include "ns3/log.h"
#include "lossless-queue-disc.h"
#include "ns3/object-factory.h"
#include "ns3/drop-tail-queue.h"
#include "ns3/ipv4-global-routing.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/udp-socket-dcqcn.h"


namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("LosslessQueueDisc");

NS_OBJECT_ENSURE_REGISTERED (LosslessQueueDisc);

TypeId LosslessQueueDisc::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::LosslessQueueDisc")
    .SetParent<QueueDisc> ()
    .SetGroupName ("TrafficControl")
    .AddConstructor<LosslessQueueDisc> ()
    .AddAttribute ("MaxSize",
                   "The max queue size. After this number is exceeded, all device MAC addresses are marked as 'off' in the 'global-onoff-info', and neighbours should stop sending to this node. However, packets that arrive after MaxSize is exceeded (due to delay or other reasons) will still be enqueued.",
                   QueueSizeValue (QueueSize ("1000p")),
                   MakeQueueSizeAccessor (&QueueDisc::SetMaxSize,
                                          &QueueDisc::GetMaxSize),
                   MakeQueueSizeChecker ())
  ;
  return tid;
}

LosslessQueueDisc::LosslessQueueDisc ()
  : QueueDisc (QueueDiscSizePolicy::SINGLE_INTERNAL_QUEUE)
{ 
  NS_LOG_FUNCTION (this);
  m_laststate = TcdState::TCD_UNDETERMINED;
  m_qState = TcdQueueState::TCD_CLEAR;
  m_start_clear_time = Simulator::Now();
  m_last_qsize = QueueSize("0p");
  m_packets_transmitted = 0;
  sem_init(&m_qlen_decrease_mutex, 0, 1);
  updateQlenDecrease();
}

LosslessQueueDisc::LosslessQueueDisc (LosslessOnoffTable* _onofftable)
  : QueueDisc (QueueDiscSizePolicy::SINGLE_INTERNAL_QUEUE)
{ // 除了SINGLE_INTERNAL_QUEUE（FIFO用的是这个），也许可以考虑试试别的？
  NS_LOG_FUNCTION (this);
  onoffTable = _onofftable;
  m_laststate = TcdState::TCD_UNDETERMINED;
  m_qState = TcdQueueState::TCD_CLEAR;
  m_start_clear_time = Simulator::Now();
  m_last_qsize = QueueSize("0p");
  m_packets_transmitted = 0;
  sem_init(&m_qlen_decrease_mutex, 0, 1);
  updateQlenDecrease();
}

LosslessQueueDisc::~LosslessQueueDisc ()
{
  NS_LOG_FUNCTION (this);
}

bool
LosslessQueueDisc::DoEnqueue (Ptr<QueueDiscItem> item)
{
  NS_LOG_FUNCTION (this << item);

  //std::cout <<"LosslessQueue "<< this <<" doEnqueue" << std :: endl;
  bool retval = GetInternalQueue (0)->Enqueue (item);

  if (GetCurrentSize () > qlenUpperBound)
    {
      //std::cout << "Device " << this << " tries to turn OFF\n";
        //TODO: 标记全局表中它的device为off。
      std :: set <Address> :: iterator it = mda.begin();
      for (; it != mda.end(); ++it) 
        onoffTable -> setValue(*it, false); 
    }

  // If Queue::Enqueue fails, QueueDisc::DropBeforeEnqueue is called by the
  // internal queue because QueueDisc::AddInternalQueue sets the trace callback

  NS_LOG_LOGIC ("Number packets " << GetInternalQueue (0)->GetNPackets ());
  NS_LOG_LOGIC ("Number bytes " << GetInternalQueue (0)->GetNBytes ());

  return retval;
}

Ptr<QueueDiscItem>
LosslessQueueDisc::DoDequeue (void)
{
  NS_LOG_FUNCTION (this);
  //std::cout <<"LosslessQueue "<< this <<" doDequeue" << std :: endl;

  // See the next packet in the queue.
  Ptr<const QueueDiscItem> item = GetInternalQueue (0)->Peek ();
  if (!item)
    {
        NS_LOG_LOGIC ("Queue empty");
        //std::cout << "DoDequeue: queue empty.\n";
        return 0;
    }
    
    //item->Print(std::cout);
  
  try {
    // Creepy pointer convertions.
    const QueueDiscItem* itemptr = &(*item);
    Ipv4QueueDiscItem* ipitem = (Ipv4QueueDiscItem*)itemptr;
    
    Ipv4Header hd = ipitem->GetHeader();

    // Find the destination device.
    Address destMAC = item -> GetAddress();
    NodeContainer nc = this->onoffTable->getGlobalNodes();
    Ptr<NetDevice> dv; // The destination device.
    Ptr<Node> destNode; // The destination node.
    bool found = 0;

    for (auto node = nc.Begin(); node!=nc.End(); node++){
      uint32_t num = (*node) -> GetNDevices();
      for (uint32_t k = 0; k + 1 != num; ++k) {
        dv = (*node)->GetDevice(k);

        if (dv->GetAddress() == destMAC){
          found = 1;
          destNode = *node;
          break;
        }
      }
      if (found == 1) break;
    }
    if (found == 0){
      throw "The device for the destination MAC is not found! (Maybe destMAC is BROADCAST?)";
    }
    
    // Find where the packet will go in the next hop.
    Socket::SocketErrno err; // The returned error number.
    Ptr<GlobalRouter> gr = destNode->GetObject<GlobalRouter>();
    Ptr<Ipv4GlobalRouting> router = gr->GetRoutingProtocol();
    // The "0" in RouteOutput's inputs is explained in the docs (https://www.nsnam.org/doxygen/classns3_1_1_ipv4_global_routing.html#a569e54ce6542c3b88305140cce134d15)
    Ptr<Ipv4Route> route = router->RouteOutput(ipitem->GetPacket(), hd, 0, err);
    //std::cout << "err: " << err << std::endl;
    //std::cout << "route: " << route << std::endl;
    if (route == NULL){
      throw "route is NULL (Reason unknown)";
    }
    Address nextQueueMAC = route->GetOutputDevice()->GetAddress();
    //std::cout << "Device address of found route: " << nextQueueMAC << "\n";
    //std::cout << "Packet with GetAddress() == " << destMAC << std::endl;
    bool destOff = ! onoffTable -> getValue(nextQueueMAC); 
    
    if (destOff) {
        //std::cout << "The destination is blocked.\n";
        NS_LOG_LOGIC ("The queue front is blocked by an OFF destination.");
        reportOutputBlocked();
        onoffTable -> blockQueueAdding(destMAC, (ns3::Ptr<ns3::QueueDisc>)this);
        /* TODO: 在对象里记录下当前队列的长度k。 */
        return 0;
        // 外部可能会因为这里返回0而认为队列空了，从而停止run。如果发现了类似的bug，要记得往这方面想（并且打补丁）。
    }
  }
  catch (const char* msg){
    //std::cout << "Caught an exception!\n";
    //std::cout << msg << "\n";
  }
  reportOutputClear();

  Ptr<QueueDiscItem> realitem = GetInternalQueue (0)->Dequeue (); // not const

  if (GetCurrentSize() <= qlenLowerBound) {
    std :: set <Address> :: iterator it = mda.begin();
    for (; it != mda.end(); ++it) 
      onoffTable -> setValue(*it, true); 
  }

  // Modify the TCD tag according to this queue's state.
  MyTag tcdTag;
  Ptr<Packet> pk = realitem->GetPacket();
  bool havetag = pk->RemovePacketTag(tcdTag);
  uint8_t tagval = 0;
  if (havetag){
    tagval |= tcdTag.GetSimpleValue();
  }

  m_packets_transmitted++;

  TcdState curTCD = getCurrentTCD();
  static int num = 0; ++num;
  std::cout <<"At Time <" << Simulator::Now ().GetSeconds () << ">, the {" << num << "}th of DoDequeue: packet=[" << pk<< "], "
            << "current TCD state is " << tcdStateName(curTCD) << ", length of queue is " << GetCurrentSize() << ".";

  switch (curTCD){
    case TcdState::TCD_CONGESTION:
      tagval |= TCD_CONGESTED_BIT;
      break;
    case TcdState::TCD_NONCONGESTION:
      tagval |= TCD_NONCONGESTED_BIT;
      break;
    case TcdState::TCD_UNDETERMINED:
      tagval |= TCD_UNDETERMINED_BIT;
      break;
  }
  tcdTag.SetSimpleValue(tagval);
  pk->AddPacketTag(tcdTag);

  return realitem;
}

Ptr<const QueueDiscItem>
LosslessQueueDisc::DoPeek (void)
{
  NS_LOG_FUNCTION (this);

  Ptr<const QueueDiscItem> item = GetInternalQueue (0)->Peek ();

  if (!item)
    {
      NS_LOG_LOGIC ("Queue empty");
      return 0;
    }

  return item;
}

bool
LosslessQueueDisc::CheckConfig (void)
{
  NS_LOG_FUNCTION (this);
  if (GetNQueueDiscClasses () > 0)
    {
      NS_LOG_ERROR ("LosslessQueueDisc cannot have classes");
      return false;
    }

  if (GetNPacketFilters () > 0)
    {
      NS_LOG_ERROR ("LosslessQueueDisc needs no packet filter");
      return false;
    }

  if (GetNInternalQueues () == 0)
    {
      // add a DropTail queue
      AddInternalQueue (CreateObjectWithAttributes<DropTailQueue<QueueDiscItem> >
                          ("MaxSize", QueueSizeValue (GetMaxSize ())));
    }

  if (GetNInternalQueues () != 1)
    {
      NS_LOG_ERROR ("LosslessQueueDisc needs 1 internal queue");
      return false;
    }

  return true;
}

void
LosslessQueueDisc::InitializeParams (void)
{
  NS_LOG_FUNCTION (this);
  blockedCnt = 0;
}

void LosslessQueueDisc::reportOutputClear(){
  if (m_qState == TcdQueueState::TCD_BLOCKED){
    // This is the first packet to send out after a BLOCKed period.
    m_qState = TcdQueueState::TCD_CLEAR;
    m_start_clear_time = Simulator::Now();
  }
  //std::cout << "LosslessQueueDisc " << this << " : A packet was successfully sent.\n";
}

void LosslessQueueDisc::reportOutputBlocked(){
  std::cout << "LosslessQueueDisc " << this << " : A packet was blocked.\n";
  m_qState = TcdQueueState::TCD_BLOCKED;
}

TcdState LosslessQueueDisc::getCurrentTCD(){
  TcdState newState = m_laststate;

  if (m_qState == TcdQueueState::TCD_BLOCKED){
    std::cout << "ERROR! This algorithm is only right when the queue is CLEAR!\n";
  }

  Time t_on = Simulator::Now() - m_start_clear_time;
  bool continuous_on = t_on > max_t_on;

  QueueSize current_qsize = GetCurrentSize();
  bool long_queue = current_qsize > tcdThreshold;
  bool qlen_decrease = getQlenDecrease();

  if (m_laststate == TcdState::TCD_NONCONGESTION){
    if (continuous_on && long_queue){
      newState = TcdState::TCD_CONGESTION;
    }
    if (!continuous_on){
      newState = TcdState::TCD_UNDETERMINED;
    }
  }
  else if (m_laststate == TcdState::TCD_CONGESTION){
    if (continuous_on && !long_queue){
      newState = TcdState::TCD_NONCONGESTION;
    }
    if (!continuous_on){
      newState = TcdState::TCD_UNDETERMINED;
    }
  }
  else if (m_laststate == TcdState::TCD_UNDETERMINED){
    if (continuous_on && (qlen_decrease || !long_queue)){
      newState = TcdState::TCD_NONCONGESTION;
    }
    if (continuous_on && (!qlen_decrease && long_queue)){
      newState = TcdState::TCD_CONGESTION;
    }
  }
  
  m_laststate = newState;
  m_last_qsize = current_qsize;
  return newState;
}

bool LosslessQueueDisc::getQlenDecrease(){
  sem_wait(&m_qlen_decrease_mutex);
  bool res = m_qlen_decrease;
  sem_post(&m_qlen_decrease_mutex);
  return res;
}

void LosslessQueueDisc::updateQlenDecrease(){
  sem_wait(&m_qlen_decrease_mutex);
  QueueSize curlen = GetCurrentSize();
  m_qlen_decrease = curlen < m_last_qsize;
  m_last_qsize = curlen;
  sem_post(&m_qlen_decrease_mutex);
  // Do this after each qsize_decrease_update_interval.
  Simulator::Schedule(qsize_decrease_update_interval, &LosslessQueueDisc::updateQlenDecrease, this);
}

int LosslessQueueDisc::getPacketsTransmitted(){
  return m_packets_transmitted;
}

std::string LosslessQueueDisc::tcdStateName(int tcd){
  switch(tcd){
    case TcdState::TCD_CONGESTION:
      return "TCD_CONGESTION";
    case TcdState::TCD_NONCONGESTION:
      return "TCD_NONCONGESTION";
    case TcdState::TCD_UNDETERMINED:
      return "TCD_UNDETERMINED";
  }
  return "What the hell??";
}

} // namespace ns3

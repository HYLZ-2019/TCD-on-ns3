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
#include "ns3/ipv4-static-routing.h"



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
}

LosslessQueueDisc::LosslessQueueDisc (LosslessOnoffTable* _onofftable)
  : QueueDisc (QueueDiscSizePolicy::SINGLE_INTERNAL_QUEUE)
{ // 除了SINGLE_INTERNAL_QUEUE（FIFO用的是这个），也许可以考虑试试别的？
  NS_LOG_FUNCTION (this);
  onoffTable = _onofftable;
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

    Address destMAC = item -> GetAddress();
    Ptr<Packet> p = item->GetPacket();//->Copy(); // Working on a copy.
    std::cout << "cout<<packet: \n";
    std::cout << p;
    //p->GetIpv4Header(std::cout);
    std::cout << "p->print: [";
    p->Print(std::cout);
    std::cout << "]\n";
    std::cout << "item->print: [";
    item->Print(std::cout);
    std::cout << "]\n";
    
    /*std::cout << p << std::endl;*/
    /*Ptr<Ipv4Route> route = Ipv4StaticRouting::RouteOutput(item->GetPacket(), )*/
    std::cout << "Packet with GetAddress() == " << destMAC << std::endl;
    bool destOff = ! onoffTable -> getValue(destMAC); 
    
    if (destOff) {
        //std::cout << "The destination is blocked.\n";
        NS_LOG_LOGIC ("The queue front is blocked by an OFF destination.");
        //onoffTable->printAll();
        /* TODO: 把“this因为destOff而停住了”记录在全局表里，以便onoff表变on的时候，可以重新run这个queue。 */
        onoffTable -> blockQueueAdding(destMAC, (ns3::Ptr<ns3::QueueDisc>)this);
        /* TODO: 在对象里记录下当前队列的长度k。 */
        return 0;
        // 外部可能会因为这里返回0而认为队列空了，从而停止run。如果发现了类似的bug，要记得往这方面想（并且打补丁）。
    }

  Ptr<QueueDiscItem> realitem = GetInternalQueue (0)->Dequeue (); // not const

  if (GetCurrentSize() <= qlenLowerBound) {
    //std::cout << "Device " << this << " tries to turn ON\n";
    /*TODO: 找到这个node上所有的device, 标记ON*/
    std :: set <Address> :: iterator it = mda.begin();
    for (; it != mda.end(); ++it) 
      onoffTable -> setValue(*it, true); 
  }

  if (blockedCnt > 0){
      /**
       * TODO: 给realitem打上TCD标记。
       * 
       */
      blockedCnt--;
  }

  //std::cout << "Returning item: " << realitem << "\n";
  //std::cout<< "realitem GetAddress(): " << realitem->GetAddress()<<std::endl;
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

} // namespace ns3

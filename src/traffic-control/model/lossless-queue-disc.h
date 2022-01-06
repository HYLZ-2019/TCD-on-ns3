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

#ifndef FIFO_QUEUE_DISC_H
#define FIFO_QUEUE_DISC_H

#include "ns3/queue-disc.h"
#include "lossless-onoff-table.h"

namespace ns3 {

/**
 * \ingroup traffic-control
 *
 * A one-queue QueueDisc for implementation of a Lossless Network.
 * When the queue length exceeds MaxSize, the node sets its devices to "OFF" in a "global-onoff-info" object. Seeing this, its neighbours will stop sending new packets to it. (However, exceeded packets won't be dropped.)
 */
class LosslessQueueDisc : public QueueDisc {
public:
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);
  /**
   * \brief LosslessQueueDisc constructor
   *
   * Creates a queue with a depth of 1000 packets by default
   */
  LosslessQueueDisc (LosslessOnoffTable _onofftable);

  LosslessQueueDisc ();

  virtual ~LosslessQueueDisc();
  
  /**
   * @brief The remaining count of packets that were once blocked in this queue and still in here.
   * 
   */
  int blockedCnt;
  
  QueueSize qlenLowerBound = QueueSize("50p");
  QueueSize qlenUpperBound = QueueSize("80p");
private:
  virtual bool DoEnqueue (Ptr<QueueDiscItem> item);
  virtual Ptr<QueueDiscItem> DoDequeue (void);
  virtual Ptr<const QueueDiscItem> DoPeek (void);
  virtual bool CheckConfig (void);
  virtual void InitializeParams (void);
};

} // namespace ns3

#endif /* FIFO_QUEUE_DISC_H */

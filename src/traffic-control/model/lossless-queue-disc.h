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

#ifndef LOSSLESS_QUEUE_DISC_H
#define LOSSLESS_QUEUE_DISC_H

#include "ns3/queue-disc.h"
#include "lossless-onoff-table.h"


namespace ns3 {

enum TcdState {
  TCD_UNDETERMINED,
  TCD_CONGESTION,
  TCD_NONCONGESTION
};

enum TcdQueueState {
  TCD_CLEAR,
  TCD_BLOCKED
};

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
  LosslessQueueDisc ();

  LosslessQueueDisc (LosslessOnoffTable* _onofftable);

  virtual ~LosslessQueueDisc();
  
  /**
   * @brief The remaining count of packets that were once blocked in this queue and still in here.
   * 
   */
  int blockedCnt;
  
  QueueSize qlenLowerBound = QueueSize("5p");
  QueueSize qlenUpperBound = QueueSize("10p");
  QueueSize tcdThreshold = QueueSize("4p");
  Time max_t_on = Time("1s");
  Time qsize_decrease_update_interval = Time("0.5s");

  /**
   * @brief Update m_qState and m_start_clear_time when a packet is clear to send.
   */
  void reportOutputClear();

  /**
   * @brief Report that a packet is blocked and update states.
   */
  void reportOutputBlocked();

  /**
   * @brief Get the current TCD state and update m_laststate.
   */
  TcdState getCurrentTCD();


  bool getQlenDecrease();

  void updateQlenDecrease();

private:
  virtual bool DoEnqueue (Ptr<QueueDiscItem> item);
  virtual Ptr<QueueDiscItem> DoDequeue (void);
  virtual Ptr<const QueueDiscItem> DoPeek (void);
  virtual bool CheckConfig (void);
  virtual void InitializeParams (void);
  TcdState m_laststate;
  // The most recent time when the queue restarted output from previous block.
  Time m_start_clear_time;
  TcdQueueState m_qState;
  QueueSize m_last_qsize;
  sem_t m_qlen_decrease_mutex;
  // Whether the queue length has been decreasing.
  bool m_qlen_decrease;
};

} // namespace ns3

#endif /* LOSSLESS_QUEUE_DISC_H */

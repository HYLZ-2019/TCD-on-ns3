#include "tcp-lossless-tcd.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3 {
    NS_LOG_COMPONENT_DEFINE ("TcpLosslessTCD");
    NS_OBJECT_ENSURE_REGISTERED (TcpLosslessTCD);

    TypeId
    TcpLosslessTCD::GetTypeId (void) {
        static TypeId tid = TypeId ("ns3::TcpLosslessTCD")
            .SetParent<TcpCongestionOps> ()
            .SetGroupName ("Internet")
            .AddConstructor<TcpLosslessTCD> ()
        ;
        return tid;
    }

    TcpLosslessTCD::TcpLosslessTCD(void) : TcpCongestionOps () {
        NS_LOG_FUNCTION (this);
    }
    
    TcpLosslessTCD::TcpLosslessTCD (const TcpLosslessTCD& sock)
        : TcpCongestionOps (sock) {
        NS_LOG_FUNCTION (this);
    }

    TcpLosslessTCD::~TcpLosslessTCD (void) {
    }

    uint32_t
    TcpLosslessTCD::SlowStart (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) {
        NS_LOG_FUNCTION (this << tcb << segmentsAcked);

        if (segmentsAcked >= 1)
            {
            uint32_t sndCwnd = tcb->m_cWnd;
            tcb->m_cWnd = std::min ((sndCwnd + (segmentsAcked * tcb->m_segmentSize)), (uint32_t)tcb->m_ssThresh);
            NS_LOG_INFO ("In SlowStart, updated to cwnd " << tcb->m_cWnd << " ssthresh " << tcb->m_ssThresh);
            return segmentsAcked - ((tcb->m_cWnd - sndCwnd) / tcb->m_segmentSize);
            }

        return 0;
    }

    void
    TcpLosslessTCD::CongestionAvoidance (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) {
        NS_LOG_FUNCTION (this << tcb << segmentsAcked);

        uint32_t w = tcb->m_cWnd / tcb->m_segmentSize;

        // Floor w to 1 if w == 0 
        if (w == 0)
            {
            w = 1;
            }

        NS_LOG_DEBUG ("w in segments " << w << " m_cWndCnt " << m_cWndCnt << " segments acked " << segmentsAcked);
        if (m_cWndCnt >= w)
            {
            m_cWndCnt = 0;
            tcb->m_cWnd += tcb->m_segmentSize;
            NS_LOG_DEBUG ("Adding 1 segment to m_cWnd");
            }

        m_cWndCnt += segmentsAcked;
        NS_LOG_DEBUG ("Adding 1 segment to m_cWndCnt");
        if (m_cWndCnt >= w)
            {
            uint32_t delta = m_cWndCnt / w;

            m_cWndCnt -= delta * w;
            tcb->m_cWnd += delta * tcb->m_segmentSize;
            NS_LOG_DEBUG ("Subtracting delta * w from m_cWndCnt " << delta * w);
            }
        NS_LOG_DEBUG ("At end of CongestionAvoidance(), m_cWnd: " << tcb->m_cWnd << " m_cWndCnt: " << m_cWndCnt);
    }

    void
    TcpLosslessTCD::IncreaseWindow (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) {
        NS_LOG_FUNCTION (this << tcb << segmentsAcked);

        // Linux tcp_in_slow_start() condition
        if (tcb->m_cWnd < tcb->m_ssThresh)
            {
            NS_LOG_DEBUG ("In slow start, m_cWnd " << tcb->m_cWnd << " m_ssThresh " << tcb->m_ssThresh);
            segmentsAcked = SlowStart (tcb, segmentsAcked);
            }
        else
            {
            NS_LOG_DEBUG ("In cong. avoidance, m_cWnd " << tcb->m_cWnd << " m_ssThresh " << tcb->m_ssThresh);
            CongestionAvoidance (tcb, segmentsAcked);
            }
    }

    std::string
    TcpLosslessTCD::GetName () const {
        return "TcpLosslessTCD";
    }

    uint32_t
    TcpLosslessTCD::GetSsThresh (Ptr<const TcpSocketState> state,
                            uint32_t bytesInFlight) {
        NS_LOG_FUNCTION (this << state << bytesInFlight);

        // In Linux, it is written as:  return max(tp->snd_cwnd >> 1U, 2U);
        return std::max<uint32_t> (2 * state->m_segmentSize, state->m_cWnd / 2);
    }

    Ptr<TcpCongestionOps>
    TcpLosslessTCD::Fork () {
        return CopyObject<TcpLosslessTCD> (this);
    }

}
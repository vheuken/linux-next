/*
 * Copyright (c) 1996, 2003 VIA Networking Technologies, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * File: rxtx.c
 *
 * Purpose: handle WMAC/802.3/802.11 rx & tx functions
 *
 * Author: Lyndon Chen
 *
 * Date: May 20, 2003
 *
 * Functions:
 *      s_vGenerateTxParameter - Generate tx dma required parameter.
 *      s_vGenerateMACHeader - Translate 802.3 to 802.11 header
 *      csBeacon_xmit - beacon tx function
 *      csMgmt_xmit - management tx function
 *      s_uGetDataDuration - get tx data required duration
 *      s_uFillDataHead- fulfill tx data duration header
 *      s_uGetRTSCTSDuration- get rtx/cts required duration
 *      s_uGetRTSCTSRsvTime- get rts/cts reserved time
 *      s_uGetTxRsvTime- get frame reserved time
 *      s_vFillCTSHead- fulfill CTS ctl header
 *      s_vFillFragParameter- Set fragment ctl parameter.
 *      s_vFillRTSHead- fulfill RTS ctl header
 *      s_vFillTxKey- fulfill tx encrypt key
 *      s_vSWencryption- Software encrypt header
 *      vDMA0_tx_80211- tx 802.11 frame via dma0
 *      vGenerateFIFOHeader- Generate tx FIFO ctl header
 *
 * Revision History:
 *
 */

#include "device.h"
#include "rxtx.h"
#include "tether.h"
#include "card.h"
#include "bssdb.h"
#include "mac.h"
#include "michael.h"
#include "tkip.h"
#include "wctl.h"
#include "rf.h"
#include "datarate.h"
#include "usbpipe.h"
#include "iocmd.h"

static int          msglevel                = MSG_LEVEL_INFO;

static const u16 wTimeStampOff[2][MAX_RATE] = {
        {384, 288, 226, 209, 54, 43, 37, 31, 28, 25, 24, 23}, // Long Preamble
        {384, 192, 130, 113, 54, 43, 37, 31, 28, 25, 24, 23}, // Short Preamble
    };

static const u16 wFB_Opt0[2][5] = {
        {RATE_12M, RATE_18M, RATE_24M, RATE_36M, RATE_48M}, // fallback_rate0
        {RATE_12M, RATE_12M, RATE_18M, RATE_24M, RATE_36M}, // fallback_rate1
    };
static const u16 wFB_Opt1[2][5] = {
        {RATE_12M, RATE_18M, RATE_24M, RATE_24M, RATE_36M}, // fallback_rate0
        {RATE_6M , RATE_6M,  RATE_12M, RATE_12M, RATE_18M}, // fallback_rate1
    };

#define RTSDUR_BB       0
#define RTSDUR_BA       1
#define RTSDUR_AA       2
#define CTSDUR_BA       3
#define RTSDUR_BA_F0    4
#define RTSDUR_AA_F0    5
#define RTSDUR_BA_F1    6
#define RTSDUR_AA_F1    7
#define CTSDUR_BA_F0    8
#define CTSDUR_BA_F1    9
#define DATADUR_B       10
#define DATADUR_A       11
#define DATADUR_A_F0    12
#define DATADUR_A_F1    13

static void s_vSaveTxPktInfo(struct vnt_private *pDevice, u8 byPktNum,
	u8 *pbyDestAddr, u16 wPktLength, u16 wFIFOCtl);

static struct vnt_usb_send_context *s_vGetFreeContext(struct vnt_private *);

static u16 s_vGenerateTxParameter(struct vnt_usb_send_context *tx_context,
	u8 byPktType, u16 wCurrentRate,	struct vnt_tx_buffer *tx_buffer,
	struct vnt_mic_hdr **mic_hdr, u32 need_mic, u32 cbFrameSize,
	int bNeedACK, struct ethhdr *psEthHeader, bool need_rts);

static void s_vGenerateMACHeader(struct vnt_private *pDevice,
	struct ieee80211_hdr *pMACHeader, u16 wDuration,
	struct ethhdr *psEthHeader, int bNeedEncrypt, u16 wFragType,
	u32 uFragIdx);

static void s_vFillTxKey(struct vnt_usb_send_context *tx_context,
	struct vnt_tx_fifo_head *fifo_head, u8 *pbyIVHead,
	PSKeyItem pTransmitKey, u16 wPayloadLen, struct vnt_mic_hdr *mic_hdr);

static void s_vSWencryption(struct vnt_private *pDevice,
	PSKeyItem pTransmitKey, u8 *pbyPayloadHead, u16 wPayloadSize);

static unsigned int s_uGetTxRsvTime(struct vnt_private *pDevice, u8 byPktType,
	u32 cbFrameLength, u16 wRate, int bNeedAck);

static __le16 s_uGetRTSCTSRsvTime(struct vnt_private *priv,
	u8 rsv_type, u8 pkt_type, u32 frame_length, u16 current_rate);

static u16 s_vFillCTSHead(struct vnt_usb_send_context *tx_context,
	u8 byPktType, union vnt_tx_data_head *head, u32 cbFrameLength,
	int bNeedAck, u16 wCurrentRate, u8 byFBOption);

static u16 s_vFillRTSHead(struct vnt_usb_send_context *tx_context, u8 byPktType,
	union vnt_tx_data_head *head, u32 cbFrameLength, int bNeedAck,
	struct ethhdr *psEthHeader, u16 wCurrentRate, u8 byFBOption);

static __le16 s_uGetDataDuration(struct vnt_private *pDevice,
	u8 byPktType, int bNeedAck);

static __le16 s_uGetRTSCTSDuration(struct vnt_private *pDevice,
	u8 byDurType, u32 cbFrameLength, u8 byPktType, u16 wRate,
	int bNeedAck, u8 byFBOption);

static struct vnt_usb_send_context
	*s_vGetFreeContext(struct vnt_private *priv)
{
	struct vnt_usb_send_context *context = NULL;
	int ii;

	DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"GetFreeContext()\n");

	for (ii = 0; ii < priv->cbTD; ii++) {
		if (!priv->apTD[ii])
			return NULL;

		context = priv->apTD[ii];
		if (context->in_use == false) {
			context->in_use = true;
			memset(context->data, 0,
					MAX_TOTAL_SIZE_WITH_ALL_HEADERS);

			context->hdr = NULL;

			return context;
		}
	}

	if (ii == priv->cbTD)
		DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"No Free Tx Context\n");

	return NULL;
}

static void s_vSaveTxPktInfo(struct vnt_private *pDevice, u8 byPktNum,
	u8 *pbyDestAddr, u16 wPktLength, u16 wFIFOCtl)
{
	struct net_device_stats *stats = &pDevice->stats;
	struct vnt_tx_pkt_info *pkt_info = pDevice->pkt_info;

	pkt_info[byPktNum].fifo_ctl = wFIFOCtl;
	memcpy(pkt_info[byPktNum].dest_addr, pbyDestAddr, ETH_ALEN);

	stats->tx_bytes += wPktLength;
}

static void s_vFillTxKey(struct vnt_usb_send_context *tx_context,
	struct vnt_tx_fifo_head *fifo_head, u8 *pbyIVHead,
	PSKeyItem pTransmitKey, u16 wPayloadLen, struct vnt_mic_hdr *mic_hdr)
{
	struct vnt_private *pDevice = tx_context->priv;
	struct ieee80211_hdr *pMACHeader = tx_context->hdr;
	u8 *pbyBuf = fifo_head->tx_key;
	__le32 *pdwIV = (__le32 *)pbyIVHead;
	__le32 *pdwExtIV = (__le32 *)((u8 *)pbyIVHead + 4);
	__le32 rev_iv_counter;

	/* Fill TXKEY */
	if (pTransmitKey == NULL)
		return;

	rev_iv_counter = cpu_to_le32(pDevice->dwIVCounter);
	*pdwIV = cpu_to_le32(pDevice->dwIVCounter);
	pDevice->byKeyIndex = pTransmitKey->dwKeyIndex & 0xf;

	switch (pTransmitKey->byCipherSuite) {
	case KEY_CTL_WEP:
		if (pTransmitKey->uKeyLength == WLAN_WEP232_KEYLEN) {
			memcpy(pDevice->abyPRNG, (u8 *)&rev_iv_counter, 3);
			memcpy(pDevice->abyPRNG + 3, pTransmitKey->abyKey,
						pTransmitKey->uKeyLength);
		} else {
			memcpy(pbyBuf, (u8 *)&rev_iv_counter, 3);
			memcpy(pbyBuf + 3, pTransmitKey->abyKey,
						pTransmitKey->uKeyLength);
			if (pTransmitKey->uKeyLength == WLAN_WEP40_KEYLEN) {
				memcpy(pbyBuf+8, (u8 *)&rev_iv_counter, 3);
				memcpy(pbyBuf+11, pTransmitKey->abyKey,
						pTransmitKey->uKeyLength);
			}

			memcpy(pDevice->abyPRNG, pbyBuf, 16);
		}
		/* Append IV after Mac Header */
		*pdwIV &= cpu_to_le32(WEP_IV_MASK);
		*pdwIV |= cpu_to_le32((u32)pDevice->byKeyIndex << 30);

		pDevice->dwIVCounter++;
		if (pDevice->dwIVCounter > WEP_IV_MASK)
			pDevice->dwIVCounter = 0;

		break;
	case KEY_CTL_TKIP:
		pTransmitKey->wTSC15_0++;
		if (pTransmitKey->wTSC15_0 == 0)
			pTransmitKey->dwTSC47_16++;

		TKIPvMixKey(pTransmitKey->abyKey, pDevice->abyCurrentNetAddr,
			pTransmitKey->wTSC15_0, pTransmitKey->dwTSC47_16,
							pDevice->abyPRNG);
		memcpy(pbyBuf, pDevice->abyPRNG, 16);

		/* Make IV */
		memcpy(pdwIV, pDevice->abyPRNG, 3);

		*(pbyIVHead+3) = (u8)(((pDevice->byKeyIndex << 6) &
							0xc0) | 0x20);
		/*  Append IV&ExtIV after Mac Header */
		*pdwExtIV = cpu_to_le32(pTransmitKey->dwTSC47_16);

		DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO
			"vFillTxKey()---- pdwExtIV: %x\n", *pdwExtIV);

		break;
	case KEY_CTL_CCMP:
		pTransmitKey->wTSC15_0++;
		if (pTransmitKey->wTSC15_0 == 0)
			pTransmitKey->dwTSC47_16++;

		memcpy(pbyBuf, pTransmitKey->abyKey, 16);

		/* Make IV */
		*pdwIV = 0;
		*(pbyIVHead+3) = (u8)(((pDevice->byKeyIndex << 6) &
							0xc0) | 0x20);

		*pdwIV |= cpu_to_le32((u32)(pTransmitKey->wTSC15_0));

		/* Append IV&ExtIV after Mac Header */
		*pdwExtIV = cpu_to_le32(pTransmitKey->dwTSC47_16);

		if (!mic_hdr)
			return;

		/* MICHDR0 */
		mic_hdr->id = 0x59;
		mic_hdr->payload_len = cpu_to_be16(wPayloadLen);
		memcpy(mic_hdr->mic_addr2, pMACHeader->addr2, ETH_ALEN);

		mic_hdr->tsc_47_16 = cpu_to_be32(pTransmitKey->dwTSC47_16);
		mic_hdr->tsc_15_0 = cpu_to_be16(pTransmitKey->wTSC15_0);

		/* MICHDR1 */
		if (ieee80211_has_a4(pMACHeader->frame_control))
			mic_hdr->hlen = cpu_to_be16(28);
		else
			mic_hdr->hlen = cpu_to_be16(22);

		memcpy(mic_hdr->addr1, pMACHeader->addr1, ETH_ALEN);
		memcpy(mic_hdr->addr2, pMACHeader->addr2, ETH_ALEN);

		/* MICHDR2 */
		memcpy(mic_hdr->addr3, pMACHeader->addr3, ETH_ALEN);
		mic_hdr->frame_control = cpu_to_le16(
			le16_to_cpu(pMACHeader->frame_control) & 0xc78f);
		mic_hdr->seq_ctrl = cpu_to_le16(
				le16_to_cpu(pMACHeader->seq_ctrl) & 0xf);

		if (ieee80211_has_a4(pMACHeader->frame_control))
			memcpy(mic_hdr->addr4, pMACHeader->addr4, ETH_ALEN);
	}
}

static void s_vSWencryption(struct vnt_private *pDevice,
	PSKeyItem pTransmitKey, u8 *pbyPayloadHead, u16 wPayloadSize)
{
	u32 cbICVlen = 4;
	u32 dwICV = 0xffffffff;
	u32 *pdwICV;

    if (pTransmitKey == NULL)
        return;

    if (pTransmitKey->byCipherSuite == KEY_CTL_WEP) {
        //=======================================================================
        // Append ICV after payload
	dwICV = ether_crc_le(wPayloadSize, pbyPayloadHead);
        pdwICV = (u32 *)(pbyPayloadHead + wPayloadSize);
        // finally, we must invert dwCRC to get the correct answer
        *pdwICV = cpu_to_le32(~dwICV);
        // RC4 encryption
        rc4_init(&pDevice->SBox, pDevice->abyPRNG, pTransmitKey->uKeyLength + 3);
        rc4_encrypt(&pDevice->SBox, pbyPayloadHead, pbyPayloadHead, wPayloadSize+cbICVlen);
        //=======================================================================
    } else if (pTransmitKey->byCipherSuite == KEY_CTL_TKIP) {
        //=======================================================================
        //Append ICV after payload
	dwICV = ether_crc_le(wPayloadSize, pbyPayloadHead);
        pdwICV = (u32 *)(pbyPayloadHead + wPayloadSize);
        // finally, we must invert dwCRC to get the correct answer
        *pdwICV = cpu_to_le32(~dwICV);
        // RC4 encryption
        rc4_init(&pDevice->SBox, pDevice->abyPRNG, TKIP_KEY_LEN);
        rc4_encrypt(&pDevice->SBox, pbyPayloadHead, pbyPayloadHead, wPayloadSize+cbICVlen);
        //=======================================================================
    }
}

static __le16 vnt_time_stamp_off(struct vnt_private *priv, u16 rate)
{
	return cpu_to_le16(wTimeStampOff[priv->byPreambleType % 2]
							[rate % MAX_RATE]);
}

/*byPktType : PK_TYPE_11A     0
             PK_TYPE_11B     1
             PK_TYPE_11GB    2
             PK_TYPE_11GA    3
*/
static u32 s_uGetTxRsvTime(struct vnt_private *priv, u8 pkt_type,
	u32 frame_length, u16 rate, int need_ack)
{
	u32 data_time, ack_time;

	data_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
							frame_length, rate);

	if (pkt_type == PK_TYPE_11B)
		ack_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
					14, (u16)priv->byTopCCKBasicRate);
	else
		ack_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
					14, (u16)priv->byTopOFDMBasicRate);

	if (need_ack)
		return data_time + priv->uSIFS + ack_time;

	return data_time;
}

static __le16 vnt_rxtx_rsvtime_le16(struct vnt_private *priv, u8 pkt_type,
	u32 frame_length, u16 rate, int need_ack)
{
	return cpu_to_le16((u16)s_uGetTxRsvTime(priv, pkt_type,
		frame_length, rate, need_ack));
}

//byFreqType: 0=>5GHZ 1=>2.4GHZ
static __le16 s_uGetRTSCTSRsvTime(struct vnt_private *priv,
	u8 rsv_type, u8 pkt_type, u32 frame_length, u16 current_rate)
{
	u32 rrv_time, rts_time, cts_time, ack_time, data_time;

	rrv_time = rts_time = cts_time = ack_time = data_time = 0;

	data_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
						frame_length, current_rate);

	if (rsv_type == 0) {
		rts_time = vnt_get_frame_time(priv->byPreambleType,
			pkt_type, 20, priv->byTopCCKBasicRate);
		cts_time = ack_time = vnt_get_frame_time(priv->byPreambleType,
			pkt_type, 14, priv->byTopCCKBasicRate);
	} else if (rsv_type == 1) {
		rts_time = vnt_get_frame_time(priv->byPreambleType,
			pkt_type, 20, priv->byTopCCKBasicRate);
		cts_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			14, priv->byTopCCKBasicRate);
		ack_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			14, priv->byTopOFDMBasicRate);
	} else if (rsv_type == 2) {
		rts_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			20, priv->byTopOFDMBasicRate);
		cts_time = ack_time = vnt_get_frame_time(priv->byPreambleType,
			pkt_type, 14, priv->byTopOFDMBasicRate);
	} else if (rsv_type == 3) {
		cts_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			14, priv->byTopCCKBasicRate);
		ack_time = vnt_get_frame_time(priv->byPreambleType, pkt_type,
			14, priv->byTopOFDMBasicRate);

		rrv_time = cts_time + ack_time + data_time + 2 * priv->uSIFS;

		return cpu_to_le16((u16)rrv_time);
	}

	rrv_time = rts_time + cts_time + ack_time + data_time + 3 * priv->uSIFS;

	return cpu_to_le16((u16)rrv_time);
}

//byFreqType 0: 5GHz, 1:2.4Ghz
static __le16 s_uGetDataDuration(struct vnt_private *pDevice,
					u8 byPktType, int bNeedAck)
{
	u32 uAckTime = 0;

	if (bNeedAck) {
		if (byPktType == PK_TYPE_11B)
			uAckTime = vnt_get_frame_time(pDevice->byPreambleType,
				byPktType, 14, pDevice->byTopCCKBasicRate);
		else
			uAckTime = vnt_get_frame_time(pDevice->byPreambleType,
				byPktType, 14, pDevice->byTopOFDMBasicRate);
		return cpu_to_le16((u16)(pDevice->uSIFS + uAckTime));
	}

	return 0;
}

//byFreqType: 0=>5GHZ 1=>2.4GHZ
static __le16 s_uGetRTSCTSDuration(struct vnt_private *pDevice, u8 byDurType,
	u32 cbFrameLength, u8 byPktType, u16 wRate, int bNeedAck,
	u8 byFBOption)
{
	u32 uCTSTime = 0, uDurTime = 0;

	switch (byDurType) {
	case RTSDUR_BB:
	case RTSDUR_BA:
	case RTSDUR_BA_F0:
	case RTSDUR_BA_F1:
		uCTSTime = vnt_get_frame_time(pDevice->byPreambleType,
				byPktType, 14, pDevice->byTopCCKBasicRate);
		uDurTime = uCTSTime + 2 * pDevice->uSIFS +
			s_uGetTxRsvTime(pDevice, byPktType,
						cbFrameLength, wRate, bNeedAck);
		break;

	case RTSDUR_AA:
	case RTSDUR_AA_F0:
	case RTSDUR_AA_F1:
		uCTSTime = vnt_get_frame_time(pDevice->byPreambleType,
				byPktType, 14, pDevice->byTopOFDMBasicRate);
		uDurTime = uCTSTime + 2 * pDevice->uSIFS +
			s_uGetTxRsvTime(pDevice, byPktType,
						cbFrameLength, wRate, bNeedAck);
		break;

	case CTSDUR_BA:
	case CTSDUR_BA_F0:
	case CTSDUR_BA_F1:
		uDurTime = pDevice->uSIFS + s_uGetTxRsvTime(pDevice,
				byPktType, cbFrameLength, wRate, bNeedAck);
		break;

	default:
		break;
	}

	return cpu_to_le16((u16)uDurTime);
}

static u16 vnt_mac_hdr_pos(struct vnt_usb_send_context *tx_context,
	struct ieee80211_hdr *hdr)
{
	u8 *head = tx_context->data + offsetof(struct vnt_tx_buffer, fifo_head);
	u8 *hdr_pos = (u8 *)hdr;

	tx_context->hdr = hdr;
	if (!tx_context->hdr)
		return 0;

	return (u16)(hdr_pos - head);
}

static u16 vnt_rxtx_datahead_g(struct vnt_usb_send_context *tx_context,
		u8 pkt_type, u16 rate, struct vnt_tx_datahead_g *buf,
		u32 frame_len, int need_ack)
{

	struct vnt_private *priv = tx_context->priv;

	/* Get SignalField,ServiceField,Length */
	vnt_get_phy_field(priv, frame_len, rate, pkt_type, &buf->a);
	vnt_get_phy_field(priv, frame_len, priv->byTopCCKBasicRate,
							PK_TYPE_11B, &buf->b);

	/* Get Duration and TimeStamp */
	buf->duration_a = s_uGetDataDuration(priv, pkt_type, need_ack);
	buf->duration_b = s_uGetDataDuration(priv, PK_TYPE_11B, need_ack);

	buf->time_stamp_off_a = vnt_time_stamp_off(priv, rate);
	buf->time_stamp_off_b = vnt_time_stamp_off(priv,
					priv->byTopCCKBasicRate);

	tx_context->tx_hdr_size = vnt_mac_hdr_pos(tx_context, &buf->hdr);

	return le16_to_cpu(buf->duration_a);
}

static u16 vnt_rxtx_datahead_g_fb(struct vnt_usb_send_context *tx_context,
		u8 pkt_type, u16 rate, struct vnt_tx_datahead_g_fb *buf,
		u32 frame_len, int need_ack)
{
	struct vnt_private *priv = tx_context->priv;

	/* Get SignalField,ServiceField,Length */
	vnt_get_phy_field(priv, frame_len, rate, pkt_type, &buf->a);

	vnt_get_phy_field(priv, frame_len, priv->byTopCCKBasicRate,
						PK_TYPE_11B, &buf->b);

	/* Get Duration and TimeStamp */
	buf->duration_a = s_uGetDataDuration(priv, pkt_type, need_ack);
	buf->duration_b = s_uGetDataDuration(priv, PK_TYPE_11B, need_ack);

	buf->duration_a_f0 = s_uGetDataDuration(priv, pkt_type, need_ack);
	buf->duration_a_f1 = s_uGetDataDuration(priv, pkt_type, need_ack);

	buf->time_stamp_off_a = vnt_time_stamp_off(priv, rate);
	buf->time_stamp_off_b = vnt_time_stamp_off(priv,
						priv->byTopCCKBasicRate);

	tx_context->tx_hdr_size = vnt_mac_hdr_pos(tx_context, &buf->hdr);

	return le16_to_cpu(buf->duration_a);
}

static u16 vnt_rxtx_datahead_a_fb(struct vnt_usb_send_context *tx_context,
		u8 pkt_type, u16 rate, struct vnt_tx_datahead_a_fb *buf,
		u32 frame_len, int need_ack)
{
	struct vnt_private *priv = tx_context->priv;

	/* Get SignalField,ServiceField,Length */
	vnt_get_phy_field(priv, frame_len, rate, pkt_type, &buf->a);
	/* Get Duration and TimeStampOff */
	buf->duration = s_uGetDataDuration(priv, pkt_type, need_ack);

	buf->duration_f0 = s_uGetDataDuration(priv, pkt_type, need_ack);
	buf->duration_f1 = s_uGetDataDuration(priv, pkt_type, need_ack);

	buf->time_stamp_off = vnt_time_stamp_off(priv, rate);

	tx_context->tx_hdr_size = vnt_mac_hdr_pos(tx_context, &buf->hdr);

	return le16_to_cpu(buf->duration);
}

static u16 vnt_rxtx_datahead_ab(struct vnt_usb_send_context *tx_context,
		u8 pkt_type, u16 rate, struct vnt_tx_datahead_ab *buf,
		u32 frame_len, int need_ack)
{
	struct vnt_private *priv = tx_context->priv;

	/* Get SignalField,ServiceField,Length */
	vnt_get_phy_field(priv, frame_len, rate, pkt_type, &buf->ab);
	/* Get Duration and TimeStampOff */
	buf->duration = s_uGetDataDuration(priv, pkt_type, need_ack);

	buf->time_stamp_off = vnt_time_stamp_off(priv, rate);

	tx_context->tx_hdr_size = vnt_mac_hdr_pos(tx_context, &buf->hdr);

	return le16_to_cpu(buf->duration);
}

static int vnt_fill_ieee80211_rts(struct vnt_private *priv,
	struct ieee80211_rts *rts, struct ethhdr *eth_hdr,
		__le16 duration)
{
	rts->duration = duration;
	rts->frame_control =
		cpu_to_le16(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_RTS);

	if (priv->op_mode == NL80211_IFTYPE_ADHOC ||
				priv->op_mode == NL80211_IFTYPE_AP)
		memcpy(rts->ra, eth_hdr->h_dest, ETH_ALEN);
	else
		memcpy(rts->ra, priv->abyBSSID, ETH_ALEN);

	if (priv->op_mode == NL80211_IFTYPE_AP)
		memcpy(rts->ta, priv->abyBSSID, ETH_ALEN);
	else
		memcpy(rts->ta, eth_hdr->h_source, ETH_ALEN);

	return 0;
}

static u16 vnt_rxtx_rts_g_head(struct vnt_usb_send_context *tx_context,
	struct vnt_rts_g *buf, struct ethhdr *eth_hdr,
	u8 pkt_type, u32 frame_len, int need_ack,
	u16 current_rate, u8 fb_option)
{
	struct vnt_private *priv = tx_context->priv;
	u16 rts_frame_len = 20;

	vnt_get_phy_field(priv, rts_frame_len, priv->byTopCCKBasicRate,
		PK_TYPE_11B, &buf->b);
	vnt_get_phy_field(priv, rts_frame_len,
		priv->byTopOFDMBasicRate, pkt_type, &buf->a);

	buf->duration_bb = s_uGetRTSCTSDuration(priv, RTSDUR_BB, frame_len,
		PK_TYPE_11B, priv->byTopCCKBasicRate, need_ack, fb_option);
	buf->duration_aa = s_uGetRTSCTSDuration(priv, RTSDUR_AA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);
	buf->duration_ba = s_uGetRTSCTSDuration(priv, RTSDUR_BA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);

	vnt_fill_ieee80211_rts(priv, &buf->data, eth_hdr, buf->duration_aa);

	return vnt_rxtx_datahead_g(tx_context, pkt_type, current_rate,
			&buf->data_head, frame_len, need_ack);
}

static u16 vnt_rxtx_rts_g_fb_head(struct vnt_usb_send_context *tx_context,
	struct vnt_rts_g_fb *buf, struct ethhdr *eth_hdr,
	u8 pkt_type, u32 frame_len, int need_ack,
	u16 current_rate, u8 fb_option)
{
	struct vnt_private *priv = tx_context->priv;
	u16 rts_frame_len = 20;

	vnt_get_phy_field(priv, rts_frame_len, priv->byTopCCKBasicRate,
		PK_TYPE_11B, &buf->b);
	vnt_get_phy_field(priv, rts_frame_len,
		priv->byTopOFDMBasicRate, pkt_type, &buf->a);


	buf->duration_bb = s_uGetRTSCTSDuration(priv, RTSDUR_BB, frame_len,
		PK_TYPE_11B, priv->byTopCCKBasicRate, need_ack, fb_option);
	buf->duration_aa = s_uGetRTSCTSDuration(priv, RTSDUR_AA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);
	buf->duration_ba = s_uGetRTSCTSDuration(priv, RTSDUR_BA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);


	buf->rts_duration_ba_f0 = s_uGetRTSCTSDuration(priv, RTSDUR_BA_F0,
		frame_len, pkt_type, priv->tx_rate_fb0, need_ack, fb_option);
	buf->rts_duration_aa_f0 = s_uGetRTSCTSDuration(priv, RTSDUR_AA_F0,
		frame_len, pkt_type, priv->tx_rate_fb0, need_ack, fb_option);
	buf->rts_duration_ba_f1 = s_uGetRTSCTSDuration(priv, RTSDUR_BA_F1,
		frame_len, pkt_type, priv->tx_rate_fb1, need_ack, fb_option);
	buf->rts_duration_aa_f1 = s_uGetRTSCTSDuration(priv, RTSDUR_AA_F1,
		frame_len, pkt_type, priv->tx_rate_fb1, need_ack, fb_option);

	vnt_fill_ieee80211_rts(priv, &buf->data, eth_hdr, buf->duration_aa);

	return vnt_rxtx_datahead_g_fb(tx_context, pkt_type, current_rate,
			&buf->data_head, frame_len, need_ack);
}

static u16 vnt_rxtx_rts_ab_head(struct vnt_usb_send_context *tx_context,
	struct vnt_rts_ab *buf, struct ethhdr *eth_hdr,
	u8 pkt_type, u32 frame_len, int need_ack,
	u16 current_rate, u8 fb_option)
{
	struct vnt_private *priv = tx_context->priv;
	u16 rts_frame_len = 20;

	vnt_get_phy_field(priv, rts_frame_len,
		priv->byTopOFDMBasicRate, pkt_type, &buf->ab);

	buf->duration = s_uGetRTSCTSDuration(priv, RTSDUR_AA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);

	vnt_fill_ieee80211_rts(priv, &buf->data, eth_hdr, buf->duration);

	return vnt_rxtx_datahead_ab(tx_context, pkt_type, current_rate,
			&buf->data_head, frame_len, need_ack);
}

static u16 vnt_rxtx_rts_a_fb_head(struct vnt_usb_send_context *tx_context,
	struct vnt_rts_a_fb *buf, struct ethhdr *eth_hdr,
	u8 pkt_type, u32 frame_len, int need_ack,
	u16 current_rate, u8 fb_option)
{
	struct vnt_private *priv = tx_context->priv;
	u16 rts_frame_len = 20;

	vnt_get_phy_field(priv, rts_frame_len,
		priv->byTopOFDMBasicRate, pkt_type, &buf->a);

	buf->duration = s_uGetRTSCTSDuration(priv, RTSDUR_AA, frame_len,
		pkt_type, current_rate, need_ack, fb_option);

	buf->rts_duration_f0 = s_uGetRTSCTSDuration(priv, RTSDUR_AA_F0,
		frame_len, pkt_type, priv->tx_rate_fb0, need_ack, fb_option);

	buf->rts_duration_f1 = s_uGetRTSCTSDuration(priv, RTSDUR_AA_F1,
		frame_len, pkt_type, priv->tx_rate_fb1, need_ack, fb_option);

	vnt_fill_ieee80211_rts(priv, &buf->data, eth_hdr, buf->duration);

	return vnt_rxtx_datahead_a_fb(tx_context, pkt_type, current_rate,
			&buf->data_head, frame_len, need_ack);
}

static u16 s_vFillRTSHead(struct vnt_usb_send_context *tx_context, u8 byPktType,
	union vnt_tx_data_head *head, u32 cbFrameLength, int bNeedAck,
	struct ethhdr *psEthHeader, u16 wCurrentRate, u8 byFBOption)
{

	if (!head)
		return 0;

	/* Note: So far RTSHead doesn't appear in ATIM
	*	& Beacom DMA, so we don't need to take them
	*	into account.
	*	Otherwise, we need to modified codes for them.
	*/
	switch (byPktType) {
	case PK_TYPE_11GB:
	case PK_TYPE_11GA:
		if (byFBOption == AUTO_FB_NONE)
			return vnt_rxtx_rts_g_head(tx_context, &head->rts_g,
				psEthHeader, byPktType, cbFrameLength,
				bNeedAck, wCurrentRate, byFBOption);
		else
			return vnt_rxtx_rts_g_fb_head(tx_context,
				&head->rts_g_fb, psEthHeader, byPktType,
				cbFrameLength, bNeedAck, wCurrentRate,
				byFBOption);
		break;
	case PK_TYPE_11A:
		if (byFBOption) {
			return vnt_rxtx_rts_a_fb_head(tx_context,
				&head->rts_a_fb, psEthHeader, byPktType,
				cbFrameLength, bNeedAck, wCurrentRate,
				byFBOption);
			break;
		}
	case PK_TYPE_11B:
		return vnt_rxtx_rts_ab_head(tx_context, &head->rts_ab,
			psEthHeader, byPktType, cbFrameLength,
			bNeedAck, wCurrentRate, byFBOption);
	}

	return 0;
}

static u16 s_vFillCTSHead(struct vnt_usb_send_context *tx_context,
	u8 byPktType, union vnt_tx_data_head *head, u32 cbFrameLength,
	int bNeedAck, u16 wCurrentRate, u8 byFBOption)
{
	struct vnt_private *pDevice = tx_context->priv;
	u32 uCTSFrameLen = 14;

	if (!head)
		return 0;

	if (byFBOption != AUTO_FB_NONE) {
		/* Auto Fall back */
		struct vnt_cts_fb *pBuf = &head->cts_g_fb;
		/* Get SignalField,ServiceField,Length */
		vnt_get_phy_field(pDevice, uCTSFrameLen,
			pDevice->byTopCCKBasicRate, PK_TYPE_11B, &pBuf->b);
		pBuf->duration_ba = s_uGetRTSCTSDuration(pDevice, CTSDUR_BA,
			cbFrameLength, byPktType,
			wCurrentRate, bNeedAck, byFBOption);
		/* Get CTSDuration_ba_f0 */
		pBuf->cts_duration_ba_f0 = s_uGetRTSCTSDuration(pDevice,
			CTSDUR_BA_F0, cbFrameLength, byPktType,
			pDevice->tx_rate_fb0, bNeedAck, byFBOption);
		/* Get CTSDuration_ba_f1 */
		pBuf->cts_duration_ba_f1 = s_uGetRTSCTSDuration(pDevice,
			CTSDUR_BA_F1, cbFrameLength, byPktType,
			pDevice->tx_rate_fb1, bNeedAck, byFBOption);
		/* Get CTS Frame body */
		pBuf->data.duration = pBuf->duration_ba;
		pBuf->data.frame_control =
			cpu_to_le16(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_CTS);

		memcpy(pBuf->data.ra, pDevice->abyCurrentNetAddr, ETH_ALEN);

		return vnt_rxtx_datahead_g_fb(tx_context, byPktType,
				wCurrentRate, &pBuf->data_head, cbFrameLength,
				bNeedAck);
	} else {
		struct vnt_cts *pBuf = &head->cts_g;
		/* Get SignalField,ServiceField,Length */
		vnt_get_phy_field(pDevice, uCTSFrameLen,
			pDevice->byTopCCKBasicRate, PK_TYPE_11B, &pBuf->b);
		/* Get CTSDuration_ba */
		pBuf->duration_ba = s_uGetRTSCTSDuration(pDevice,
			CTSDUR_BA, cbFrameLength, byPktType,
			wCurrentRate, bNeedAck, byFBOption);
		/*Get CTS Frame body*/
		pBuf->data.duration = pBuf->duration_ba;
		pBuf->data.frame_control =
			cpu_to_le16(IEEE80211_FTYPE_CTL | IEEE80211_STYPE_CTS);

		memcpy(pBuf->data.ra, pDevice->abyCurrentNetAddr, ETH_ALEN);

		return vnt_rxtx_datahead_g(tx_context, byPktType, wCurrentRate,
				&pBuf->data_head, cbFrameLength, bNeedAck);
        }

	return 0;
}

/*+
 *
 * Description:
 *      Generate FIFO control for MAC & Baseband controller
 *
 * Parameters:
 *  In:
 *      pDevice         - Pointer to adpater
 *      pTxDataHead     - Transmit Data Buffer
 *      pTxBufHead      - pTxBufHead
 *      pvRrvTime        - pvRrvTime
 *      pvRTS            - RTS Buffer
 *      pCTS            - CTS Buffer
 *      cbFrameSize     - Transmit Data Length (Hdr+Payload+FCS)
 *      bNeedACK        - If need ACK
 *  Out:
 *      none
 *
 * Return Value: none
 *
-*/

static u16 s_vGenerateTxParameter(struct vnt_usb_send_context *tx_context,
	u8 byPktType, u16 wCurrentRate,	struct vnt_tx_buffer *tx_buffer,
	struct vnt_mic_hdr **mic_hdr, u32 need_mic, u32 cbFrameSize,
	int bNeedACK, struct ethhdr *psEthHeader, bool need_rts)
{
	struct vnt_private *pDevice = tx_context->priv;
	struct vnt_tx_fifo_head *pFifoHead = &tx_buffer->fifo_head;
	union vnt_tx_data_head *head = NULL;
	u16 wFifoCtl;
	u8 byFBOption = AUTO_FB_NONE;

	pFifoHead->current_rate = cpu_to_le16(wCurrentRate);
	wFifoCtl = pFifoHead->wFIFOCtl;

	if (wFifoCtl & FIFOCTL_AUTO_FB_0)
		byFBOption = AUTO_FB_0;
	else if (wFifoCtl & FIFOCTL_AUTO_FB_1)
		byFBOption = AUTO_FB_1;

	if (byPktType == PK_TYPE_11GB || byPktType == PK_TYPE_11GA) {
		if (need_rts) {
			struct vnt_rrv_time_rts *pBuf =
					&tx_buffer->tx_head.tx_rts.rts;

			pBuf->rts_rrv_time_aa = s_uGetRTSCTSRsvTime(pDevice, 2,
					byPktType, cbFrameSize, wCurrentRate);
			pBuf->rts_rrv_time_ba = s_uGetRTSCTSRsvTime(pDevice, 1,
					byPktType, cbFrameSize, wCurrentRate);
			pBuf->rts_rrv_time_bb = s_uGetRTSCTSRsvTime(pDevice, 0,
				byPktType, cbFrameSize, wCurrentRate);

			pBuf->rrv_time_a = vnt_rxtx_rsvtime_le16(pDevice,
				byPktType, cbFrameSize, wCurrentRate, bNeedACK);
			pBuf->rrv_time_b = vnt_rxtx_rsvtime_le16(pDevice,
					PK_TYPE_11B, cbFrameSize,
					pDevice->byTopCCKBasicRate, bNeedACK);

			if (need_mic) {
				*mic_hdr = &tx_buffer->
						tx_head.tx_rts.tx.mic.hdr;
				head = &tx_buffer->tx_head.tx_rts.tx.mic.head;
			} else {
				head = &tx_buffer->tx_head.tx_rts.tx.head;
			}

			/* Fill RTS */
			return s_vFillRTSHead(tx_context, byPktType, head,
					cbFrameSize, bNeedACK, psEthHeader,
						wCurrentRate, byFBOption);

		} else {
			struct vnt_rrv_time_cts *pBuf = &tx_buffer->
							tx_head.tx_cts.cts;

			pBuf->rrv_time_a = vnt_rxtx_rsvtime_le16(pDevice,
				byPktType, cbFrameSize, wCurrentRate, bNeedACK);
			pBuf->rrv_time_b = vnt_rxtx_rsvtime_le16(pDevice,
				PK_TYPE_11B, cbFrameSize,
					pDevice->byTopCCKBasicRate, bNeedACK);

			pBuf->cts_rrv_time_ba = s_uGetRTSCTSRsvTime(pDevice, 3,
					byPktType, cbFrameSize, wCurrentRate);

			if (need_mic) {
				*mic_hdr = &tx_buffer->
						tx_head.tx_cts.tx.mic.hdr;
				head = &tx_buffer->tx_head.tx_cts.tx.mic.head;
			} else {
				head = &tx_buffer->tx_head.tx_cts.tx.head;
			}

			/* Fill CTS */
			return s_vFillCTSHead(tx_context, byPktType,
				head, cbFrameSize, bNeedACK, wCurrentRate,
					byFBOption);
		}
	} else if (byPktType == PK_TYPE_11A) {
		if (need_mic) {
			*mic_hdr = &tx_buffer->tx_head.tx_ab.tx.mic.hdr;
			head = &tx_buffer->tx_head.tx_ab.tx.mic.head;
		} else {
			head = &tx_buffer->tx_head.tx_ab.tx.head;
		}

		if (need_rts) {
			struct vnt_rrv_time_ab *pBuf = &tx_buffer->
							tx_head.tx_ab.ab;

			pBuf->rts_rrv_time = s_uGetRTSCTSRsvTime(pDevice, 2,
				byPktType, cbFrameSize, wCurrentRate);

			pBuf->rrv_time = vnt_rxtx_rsvtime_le16(pDevice,
				byPktType, cbFrameSize, wCurrentRate, bNeedACK);

			/* Fill RTS */
			return s_vFillRTSHead(tx_context, byPktType, head,
				cbFrameSize, bNeedACK, psEthHeader,
					wCurrentRate, byFBOption);
		} else {
			struct vnt_rrv_time_ab *pBuf = &tx_buffer->
							tx_head.tx_ab.ab;

			pBuf->rrv_time = vnt_rxtx_rsvtime_le16(pDevice,
				PK_TYPE_11A, cbFrameSize,
					wCurrentRate, bNeedACK);

			return vnt_rxtx_datahead_a_fb(tx_context, byPktType,
				wCurrentRate, &head->data_head_a_fb,
						cbFrameSize, bNeedACK);
		}
	} else if (byPktType == PK_TYPE_11B) {
		if (need_mic) {
			*mic_hdr = &tx_buffer->tx_head.tx_ab.tx.mic.hdr;
			head = &tx_buffer->tx_head.tx_ab.tx.mic.head;
		} else {
			head = &tx_buffer->tx_head.tx_ab.tx.head;
		}

		if (need_rts) {
			struct vnt_rrv_time_ab *pBuf = &tx_buffer->
							tx_head.tx_ab.ab;

			pBuf->rts_rrv_time = s_uGetRTSCTSRsvTime(pDevice, 0,
				byPktType, cbFrameSize, wCurrentRate);

			pBuf->rrv_time = vnt_rxtx_rsvtime_le16(pDevice,
				PK_TYPE_11B, cbFrameSize, wCurrentRate,
								bNeedACK);

			/* Fill RTS */
			return s_vFillRTSHead(tx_context, byPktType, head,
				cbFrameSize,
			bNeedACK, psEthHeader, wCurrentRate, byFBOption);
		} else {
			struct vnt_rrv_time_ab *pBuf = &tx_buffer->
							tx_head.tx_ab.ab;

			pBuf->rrv_time = vnt_rxtx_rsvtime_le16(pDevice,
				PK_TYPE_11B, cbFrameSize,
					wCurrentRate, bNeedACK);

			return vnt_rxtx_datahead_ab(tx_context, byPktType,
				wCurrentRate, &head->data_head_ab,
					cbFrameSize, bNeedACK);
		}
	}

	return 0;
}
/*
    u8 * pbyBuffer,//point to pTxBufHead
    u16  wFragType,//00:Non-Frag, 01:Start, 02:Mid, 03:Last
    unsigned int  cbFragmentSize,//Hdr+payoad+FCS
*/

static int s_bPacketToWirelessUsb(struct vnt_usb_send_context *tx_context,
	u8 byPktType, struct vnt_tx_buffer *tx_buffer, int bNeedEncryption,
	u32 uSkbPacketLen, struct ethhdr *psEthHeader,
	u8 *pPacket, PSKeyItem pTransmitKey, u32 uNodeIndex, u16 wCurrentRate,
	u32 *pcbHeaderLen, u32 *pcbTotalLen)
{
	struct vnt_private *pDevice = tx_context->priv;
	struct vnt_tx_fifo_head *pTxBufHead = &tx_buffer->fifo_head;
	u32 cbFrameSize, cbFrameBodySize;
	u32 cb802_1_H_len;
	u32 cbIVlen = 0, cbICVlen = 0, cbMIClen = 0, cbMACHdLen = 0;
	u32 cbFCSlen = 4, cbMICHDR = 0;
	int bNeedACK;
	bool bRTS = false;
	u8 *pbyType, *pbyMacHdr, *pbyIVHead, *pbyPayloadHead;
	u8 abySNAP_RFC1042[ETH_ALEN] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
	u8 abySNAP_Bridgetunnel[ETH_ALEN]
		= {0xAA, 0xAA, 0x03, 0x00, 0x00, 0xF8};
	u32 uDuration;
	u32 cbHeaderLength = 0, uPadding = 0;
	struct vnt_mic_hdr *pMICHDR;
	u8 byFBOption = AUTO_FB_NONE, byFragType;
	u32 dwMICKey0, dwMICKey1, dwMIC_Priority;
	u32 *pdwMIC_L, *pdwMIC_R;
	int bSoftWEP = false;

	pMICHDR = NULL;

	if (bNeedEncryption && pTransmitKey->pvKeyTable) {
		if (((PSKeyTable)pTransmitKey->pvKeyTable)->bSoftWEP == true)
			bSoftWEP = true; /* WEP 256 */
	}

	/* Get pkt type */
	if (ntohs(psEthHeader->h_proto) > ETH_DATA_LEN)
		cb802_1_H_len = 8;
	else
		cb802_1_H_len = 0;

    cbFrameBodySize = uSkbPacketLen - ETH_HLEN + cb802_1_H_len;

    //Set packet type
    pTxBufHead->wFIFOCtl |= (u16)(byPktType<<8);

	if (pDevice->op_mode == NL80211_IFTYPE_ADHOC ||
			pDevice->op_mode == NL80211_IFTYPE_AP) {
		if (is_multicast_ether_addr(psEthHeader->h_dest)) {
			bNeedACK = false;
			pTxBufHead->wFIFOCtl =
				pTxBufHead->wFIFOCtl & (~FIFOCTL_NEEDACK);
		} else {
			bNeedACK = true;
			pTxBufHead->wFIFOCtl |= FIFOCTL_NEEDACK;
		}
	} else {
		/* MSDUs in Infra mode always need ACK */
		bNeedACK = true;
		pTxBufHead->wFIFOCtl |= FIFOCTL_NEEDACK;
	}

    pTxBufHead->time_stamp = cpu_to_le16(DEFAULT_MSDU_LIFETIME_RES_64us);

    //Set FRAGCTL_MACHDCNT
	cbMACHdLen = WLAN_HDR_ADDR3_LEN;

    pTxBufHead->wFragCtl |= (u16)(cbMACHdLen << 10);

    //Set FIFOCTL_GrpAckPolicy
    if (pDevice->bGrpAckPolicy == true) {//0000 0100 0000 0000
        pTxBufHead->wFIFOCtl |=	FIFOCTL_GRPACK;
    }

	/* Set Auto Fallback Ctl */
	if (wCurrentRate >= RATE_18M) {
		if (pDevice->byAutoFBCtrl == AUTO_FB_0) {
			pTxBufHead->wFIFOCtl |= FIFOCTL_AUTO_FB_0;

			pDevice->tx_rate_fb0 =
				wFB_Opt0[FB_RATE0][wCurrentRate - RATE_18M];
			pDevice->tx_rate_fb1 =
				wFB_Opt0[FB_RATE1][wCurrentRate - RATE_18M];

			byFBOption = AUTO_FB_0;
		} else if (pDevice->byAutoFBCtrl == AUTO_FB_1) {
			pTxBufHead->wFIFOCtl |= FIFOCTL_AUTO_FB_1;
			pDevice->tx_rate_fb0 =
				wFB_Opt1[FB_RATE0][wCurrentRate - RATE_18M];
			pDevice->tx_rate_fb1 =
				wFB_Opt1[FB_RATE1][wCurrentRate - RATE_18M];

			byFBOption = AUTO_FB_1;
		}
	}

    if (bSoftWEP != true) {
        if ((bNeedEncryption) && (pTransmitKey != NULL))  { //WEP enabled
            if (pTransmitKey->byCipherSuite == KEY_CTL_WEP) { //WEP40 or WEP104
                pTxBufHead->wFragCtl |= FRAGCTL_LEGACY;
            }
            if (pTransmitKey->byCipherSuite == KEY_CTL_TKIP) {
                DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"Tx Set wFragCtl == FRAGCTL_TKIP\n");
                pTxBufHead->wFragCtl |= FRAGCTL_TKIP;
            }
            else if (pTransmitKey->byCipherSuite == KEY_CTL_CCMP) { //CCMP
                pTxBufHead->wFragCtl |= FRAGCTL_AES;
            }
        }
    }

    if ((bNeedEncryption) && (pTransmitKey != NULL))  {
        if (pTransmitKey->byCipherSuite == KEY_CTL_WEP) {
            cbIVlen = 4;
            cbICVlen = 4;
        }
        else if (pTransmitKey->byCipherSuite == KEY_CTL_TKIP) {
            cbIVlen = 8;//IV+ExtIV
            cbMIClen = 8;
            cbICVlen = 4;
        }
        if (pTransmitKey->byCipherSuite == KEY_CTL_CCMP) {
            cbIVlen = 8;//RSN Header
            cbICVlen = 8;//MIC
	    cbMICHDR = sizeof(struct vnt_mic_hdr);
        }
        if (bSoftWEP == false) {
            //MAC Header should be padding 0 to DW alignment.
            uPadding = 4 - (cbMACHdLen%4);
            uPadding %= 4;
        }
    }

    cbFrameSize = cbMACHdLen + cbIVlen + (cbFrameBodySize + cbMIClen) + cbICVlen + cbFCSlen;

    if ( (bNeedACK == false) ||(cbFrameSize < pDevice->wRTSThreshold) ) {
        bRTS = false;
    } else {
        bRTS = true;
        pTxBufHead->wFIFOCtl |= (FIFOCTL_RTS | FIFOCTL_LRETRY);
    }

    //=========================
    //    No Fragmentation
    //=========================
    DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"No Fragmentation...\n");
    byFragType = FRAGCTL_NONFRAG;
    //pTxBufHead = (PSTxBufHead) &(pTxBufHead->adwTxKey[0]);

	/* Fill FIFO, RrvTime, RTS and CTS */
	uDuration = s_vGenerateTxParameter(tx_context, byPktType, wCurrentRate,
			tx_buffer, &pMICHDR, cbMICHDR,
			cbFrameSize, bNeedACK, psEthHeader, bRTS);

	cbHeaderLength = tx_context->tx_hdr_size;
	if (!cbHeaderLength)
		return false;

	pbyMacHdr = (u8 *)tx_context->hdr;
	pbyIVHead = (u8 *)(pbyMacHdr + cbMACHdLen + uPadding);
	pbyPayloadHead = (u8 *)(pbyMacHdr + cbMACHdLen + uPadding + cbIVlen);

	/* Generate TX MAC Header */
	s_vGenerateMACHeader(pDevice, tx_context->hdr, (u16)uDuration,
		psEthHeader, bNeedEncryption, byFragType, 0);

    if (bNeedEncryption == true) {
        //Fill TXKEY
	s_vFillTxKey(tx_context, pTxBufHead, pbyIVHead, pTransmitKey,
		(u16)cbFrameBodySize, pMICHDR);
    }

	/* 802.1H */
	if (ntohs(psEthHeader->h_proto) > ETH_DATA_LEN) {
		if ((psEthHeader->h_proto == cpu_to_be16(ETH_P_IPX)) ||
			(psEthHeader->h_proto == cpu_to_le16(0xF380)))
			memcpy((u8 *) (pbyPayloadHead),
					abySNAP_Bridgetunnel, 6);
		else
			memcpy((u8 *) (pbyPayloadHead), &abySNAP_RFC1042[0], 6);

		pbyType = (u8 *) (pbyPayloadHead + 6);

		memcpy(pbyType, &(psEthHeader->h_proto), sizeof(u16));
	}

    if (pPacket != NULL) {
        // Copy the Packet into a tx Buffer
        memcpy((pbyPayloadHead + cb802_1_H_len),
                 (pPacket + ETH_HLEN),
                 uSkbPacketLen - ETH_HLEN
                 );

    } else {
        // while bRelayPacketSend psEthHeader is point to header+payload
        memcpy((pbyPayloadHead + cb802_1_H_len), ((u8 *)psEthHeader) + ETH_HLEN, uSkbPacketLen - ETH_HLEN);
    }

    if ((bNeedEncryption == true) && (pTransmitKey != NULL) && (pTransmitKey->byCipherSuite == KEY_CTL_TKIP)) {

        ///////////////////////////////////////////////////////////////////

	if (pDevice->vnt_mgmt.eAuthenMode == WMAC_AUTH_WPANONE) {
		dwMICKey0 = *(u32 *)(&pTransmitKey->abyKey[16]);
		dwMICKey1 = *(u32 *)(&pTransmitKey->abyKey[20]);
	}
        else if ((pTransmitKey->dwKeyIndex & AUTHENTICATOR_KEY) != 0) {
            dwMICKey0 = *(u32 *)(&pTransmitKey->abyKey[16]);
            dwMICKey1 = *(u32 *)(&pTransmitKey->abyKey[20]);
        }
        else {
            dwMICKey0 = *(u32 *)(&pTransmitKey->abyKey[24]);
            dwMICKey1 = *(u32 *)(&pTransmitKey->abyKey[28]);
        }
        // DO Software Michael
        MIC_vInit(dwMICKey0, dwMICKey1);
        MIC_vAppend((u8 *)&(psEthHeader->h_dest[0]), 12);
        dwMIC_Priority = 0;
        MIC_vAppend((u8 *)&dwMIC_Priority, 4);
	DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"MIC KEY: %X, %X\n",
		dwMICKey0, dwMICKey1);

        ///////////////////////////////////////////////////////////////////

        //DBG_PRN_GRP12(("Length:%d, %d\n", cbFrameBodySize, uFromHDtoPLDLength));
        //for (ii = 0; ii < cbFrameBodySize; ii++) {
        //    DBG_PRN_GRP12(("%02x ", *((u8 *)((pbyPayloadHead + cb802_1_H_len) + ii))));
        //}
        //DBG_PRN_GRP12(("\n\n\n"));

        MIC_vAppend(pbyPayloadHead, cbFrameBodySize);

        pdwMIC_L = (u32 *)(pbyPayloadHead + cbFrameBodySize);
        pdwMIC_R = (u32 *)(pbyPayloadHead + cbFrameBodySize + 4);

        MIC_vGetMIC(pdwMIC_L, pdwMIC_R);
        MIC_vUnInit();

        if (pDevice->bTxMICFail == true) {
            *pdwMIC_L = 0;
            *pdwMIC_R = 0;
            pDevice->bTxMICFail = false;
        }
        //DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"uLength: %d, %d\n", uLength, cbFrameBodySize);
        //DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"cbReqCount:%d, %d, %d, %d\n", cbReqCount, cbHeaderLength, uPadding, cbIVlen);
        //DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"MIC:%lX, %lX\n", *pdwMIC_L, *pdwMIC_R);
    }

    if (bSoftWEP == true) {

        s_vSWencryption(pDevice, pTransmitKey, (pbyPayloadHead), (u16)(cbFrameBodySize + cbMIClen));

    } else if (  ((pDevice->eEncryptionStatus == Ndis802_11Encryption1Enabled) && (bNeedEncryption == true))  ||
          ((pDevice->eEncryptionStatus == Ndis802_11Encryption2Enabled) && (bNeedEncryption == true))   ||
          ((pDevice->eEncryptionStatus == Ndis802_11Encryption3Enabled) && (bNeedEncryption == true))      ) {
        cbFrameSize -= cbICVlen;
    }

        cbFrameSize -= cbFCSlen;

    *pcbHeaderLen = cbHeaderLength;
    *pcbTotalLen = cbHeaderLength + cbFrameSize ;

    //Set FragCtl in TxBufferHead
    pTxBufHead->wFragCtl |= (u16)byFragType;

    return true;

}

/*+
 *
 * Description:
 *      Translate 802.3 to 802.11 header
 *
 * Parameters:
 *  In:
 *      pDevice         - Pointer to adapter
 *      dwTxBufferAddr  - Transmit Buffer
 *      pPacket         - Packet from upper layer
 *      cbPacketSize    - Transmit Data Length
 *  Out:
 *      pcbHeadSize         - Header size of MAC&Baseband control and 802.11 Header
 *      pcbAppendPayload    - size of append payload for 802.1H translation
 *
 * Return Value: none
 *
-*/

static void s_vGenerateMACHeader(struct vnt_private *pDevice,
	struct ieee80211_hdr *pMACHeader, u16 wDuration,
	struct ethhdr *psEthHeader, int bNeedEncrypt, u16 wFragType,
	u32 uFragIdx)
{

	pMACHeader->frame_control = TYPE_802_11_DATA;

    if (pDevice->op_mode == NL80211_IFTYPE_AP) {
	memcpy(&(pMACHeader->addr1[0]),
	       &(psEthHeader->h_dest[0]),
	       ETH_ALEN);
	memcpy(&(pMACHeader->addr2[0]), &(pDevice->abyBSSID[0]), ETH_ALEN);
	memcpy(&(pMACHeader->addr3[0]),
	       &(psEthHeader->h_source[0]),
	       ETH_ALEN);
        pMACHeader->frame_control |= FC_FROMDS;
    } else {
	if (pDevice->op_mode == NL80211_IFTYPE_ADHOC) {
		memcpy(&(pMACHeader->addr1[0]),
		       &(psEthHeader->h_dest[0]),
		       ETH_ALEN);
		memcpy(&(pMACHeader->addr2[0]),
		       &(psEthHeader->h_source[0]),
		       ETH_ALEN);
		memcpy(&(pMACHeader->addr3[0]),
		       &(pDevice->abyBSSID[0]),
		       ETH_ALEN);
	} else {
		memcpy(&(pMACHeader->addr3[0]),
		       &(psEthHeader->h_dest[0]),
		       ETH_ALEN);
		memcpy(&(pMACHeader->addr2[0]),
		       &(psEthHeader->h_source[0]),
		       ETH_ALEN);
		memcpy(&(pMACHeader->addr1[0]),
		       &(pDevice->abyBSSID[0]),
		       ETH_ALEN);
            pMACHeader->frame_control |= FC_TODS;
        }
    }

    if (bNeedEncrypt)
        pMACHeader->frame_control |= cpu_to_le16((u16)WLAN_SET_FC_ISWEP(1));

    pMACHeader->duration_id = cpu_to_le16(wDuration);

    pMACHeader->seq_ctrl = cpu_to_le16(pDevice->wSeqCounter << 4);

    //Set FragNumber in Sequence Control
    pMACHeader->seq_ctrl |= cpu_to_le16((u16)uFragIdx);

    if ((wFragType == FRAGCTL_ENDFRAG) || (wFragType == FRAGCTL_NONFRAG)) {
        pDevice->wSeqCounter++;
        if (pDevice->wSeqCounter > 0x0fff)
            pDevice->wSeqCounter = 0;
    }

    if ((wFragType == FRAGCTL_STAFRAG) || (wFragType == FRAGCTL_MIDFRAG)) { //StartFrag or MidFrag
        pMACHeader->frame_control |= FC_MOREFRAG;
    }
}

/*+
 *
 * Description:
 *      Request instructs a MAC to transmit a 802.11 management packet through
 *      the adapter onto the medium.
 *
 * Parameters:
 *  In:
 *      hDeviceContext  - Pointer to the adapter
 *      pPacket         - A pointer to a descriptor for the packet to transmit
 *  Out:
 *      none
 *
 * Return Value: CMD_STATUS_PENDING if MAC Tx resource available; otherwise false
 *
-*/

CMD_STATUS csMgmt_xmit(struct vnt_private *pDevice,
	struct vnt_tx_mgmt *pPacket)
{
	struct vnt_manager *pMgmt = &pDevice->vnt_mgmt;
	struct vnt_tx_buffer *pTX_Buffer;
	struct vnt_usb_send_context *pContext;
	struct vnt_tx_fifo_head *pTxBufHead;
	struct ieee80211_hdr *pMACHeader;
	struct ethhdr sEthHeader;
	u8 byPktType, *pbyTxBufferAddr;
	struct vnt_mic_hdr *pMICHDR = NULL;
	u32 uDuration, cbReqCount, cbHeaderSize, cbFrameBodySize, cbFrameSize;
	int bNeedACK, bIsPSPOLL = false;
	u32 cbIVlen = 0, cbICVlen = 0, cbMIClen = 0, cbFCSlen = 4;
	u32 uPadding = 0;
	u16 wTxBufSize;
	u32 cbMacHdLen;
	u16 wCurrentRate = RATE_1M;
	unsigned long flags;

	if (pDevice->byBBType == BB_TYPE_11A) {
		wCurrentRate = RATE_6M;
		byPktType = PK_TYPE_11A;
	} else {
		wCurrentRate = RATE_1M;
		byPktType = PK_TYPE_11B;
	}

	if (pMgmt->eScanState != WMAC_NO_SCANNING)
		vnt_rf_setpower(pDevice, wCurrentRate, pDevice->byCurrentCh);
	else
		vnt_rf_setpower(pDevice, wCurrentRate, pMgmt->uCurrChannel);

	pDevice->wCurrentRate = wCurrentRate;

	spin_lock_irqsave(&pDevice->lock, flags);

	pContext = s_vGetFreeContext(pDevice);
	if (!pContext) {
		DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO
			"ManagementSend TX...NO CONTEXT!\n");
		spin_unlock_irqrestore(&pDevice->lock, flags);
		return CMD_STATUS_RESOURCES;
	}

	pTX_Buffer = (struct vnt_tx_buffer *)&pContext->data[0];
    cbFrameBodySize = pPacket->cbPayloadLen;
	pTxBufHead = &pTX_Buffer->fifo_head;
	pbyTxBufferAddr = (u8 *)pTxBufHead;
	wTxBufSize = sizeof(struct vnt_tx_fifo_head);


    //Set packet type
    if (byPktType == PK_TYPE_11A) {//0000 0000 0000 0000
        pTxBufHead->wFIFOCtl = 0;
    }
    else if (byPktType == PK_TYPE_11B) {//0000 0001 0000 0000
        pTxBufHead->wFIFOCtl |= FIFOCTL_11B;
    }
    else if (byPktType == PK_TYPE_11GB) {//0000 0010 0000 0000
        pTxBufHead->wFIFOCtl |= FIFOCTL_11GB;
    }
    else if (byPktType == PK_TYPE_11GA) {//0000 0011 0000 0000
        pTxBufHead->wFIFOCtl |= FIFOCTL_11GA;
    }

    pTxBufHead->wFIFOCtl |= FIFOCTL_TMOEN;
    pTxBufHead->time_stamp = cpu_to_le16(DEFAULT_MGN_LIFETIME_RES_64us);

    if (is_multicast_ether_addr(pPacket->p80211Header->sA3.abyAddr1)) {
        bNeedACK = false;
    }
    else {
        bNeedACK = true;
        pTxBufHead->wFIFOCtl |= FIFOCTL_NEEDACK;
    };

    if ((pMgmt->eCurrMode == WMAC_MODE_ESS_AP) ||
        (pMgmt->eCurrMode == WMAC_MODE_IBSS_STA) ) {

        pTxBufHead->wFIFOCtl |= FIFOCTL_LRETRY;
        //Set Preamble type always long
        //pDevice->byPreambleType = PREAMBLE_LONG;
        // probe-response don't retry
        //if ((pPacket->p80211Header->sA4.wFrameCtl & TYPE_SUBTYPE_MASK) == TYPE_MGMT_PROBE_RSP) {
        //     bNeedACK = false;
        //     pTxBufHead->wFIFOCtl  &= (~FIFOCTL_NEEDACK);
        //}
    }

    pTxBufHead->wFIFOCtl |= (FIFOCTL_GENINT | FIFOCTL_ISDMA0);

    if ((pPacket->p80211Header->sA4.wFrameCtl & TYPE_SUBTYPE_MASK) == TYPE_CTL_PSPOLL) {
        bIsPSPOLL = true;
        cbMacHdLen = WLAN_HDR_ADDR2_LEN;
    } else {
        cbMacHdLen = WLAN_HDR_ADDR3_LEN;
    }

    //Set FRAGCTL_MACHDCNT
    pTxBufHead->wFragCtl |= cpu_to_le16((u16)(cbMacHdLen << 10));

    // Notes:
    // Although spec says MMPDU can be fragmented; In most case,
    // no one will send a MMPDU under fragmentation. With RTS may occur.

    if (WLAN_GET_FC_ISWEP(pPacket->p80211Header->sA4.wFrameCtl) != 0) {
        if (pDevice->eEncryptionStatus == Ndis802_11Encryption1Enabled) {
            cbIVlen = 4;
            cbICVlen = 4;
    	    pTxBufHead->wFragCtl |= FRAGCTL_LEGACY;
        }
        else if (pDevice->eEncryptionStatus == Ndis802_11Encryption2Enabled) {
            cbIVlen = 8;//IV+ExtIV
            cbMIClen = 8;
            cbICVlen = 4;
    	    pTxBufHead->wFragCtl |= FRAGCTL_TKIP;
    	    //We need to get seed here for filling TxKey entry.
            //TKIPvMixKey(pTransmitKey->abyKey, pDevice->abyCurrentNetAddr,
            //            pTransmitKey->wTSC15_0, pTransmitKey->dwTSC47_16, pDevice->abyPRNG);
        }
        else if (pDevice->eEncryptionStatus == Ndis802_11Encryption3Enabled) {
            cbIVlen = 8;//RSN Header
            cbICVlen = 8;//MIC
            pTxBufHead->wFragCtl |= FRAGCTL_AES;
        }
        //MAC Header should be padding 0 to DW alignment.
        uPadding = 4 - (cbMacHdLen%4);
        uPadding %= 4;
    }

    cbFrameSize = cbMacHdLen + cbFrameBodySize + cbIVlen + cbMIClen + cbICVlen + cbFCSlen;

    //Set FIFOCTL_GrpAckPolicy
    if (pDevice->bGrpAckPolicy == true) {//0000 0100 0000 0000
        pTxBufHead->wFIFOCtl |=	FIFOCTL_GRPACK;
    }
    //the rest of pTxBufHead->wFragCtl:FragTyp will be set later in s_vFillFragParameter()

    memcpy(&(sEthHeader.h_dest[0]),
	   &(pPacket->p80211Header->sA3.abyAddr1[0]),
	   ETH_ALEN);
    memcpy(&(sEthHeader.h_source[0]),
	   &(pPacket->p80211Header->sA3.abyAddr2[0]),
	   ETH_ALEN);
    //=========================
    //    No Fragmentation
    //=========================
    pTxBufHead->wFragCtl |= (u16)FRAGCTL_NONFRAG;

	/* Fill FIFO,RrvTime,RTS,and CTS */
	uDuration = s_vGenerateTxParameter(pContext, byPktType, wCurrentRate,
		pTX_Buffer, &pMICHDR, 0,
		cbFrameSize, bNeedACK, &sEthHeader, false);

	cbHeaderSize = pContext->tx_hdr_size;
	if (!cbHeaderSize) {
		pContext->in_use = false;
		return CMD_STATUS_RESOURCES;
	}

	pMACHeader = pContext->hdr;

    cbReqCount = cbHeaderSize + cbMacHdLen + uPadding + cbIVlen + cbFrameBodySize;

    if (WLAN_GET_FC_ISWEP(pPacket->p80211Header->sA4.wFrameCtl) != 0) {
        u8 *           pbyIVHead;
        u8 *           pbyPayloadHead;
        u8 *           pbyBSSID;
        PSKeyItem       pTransmitKey = NULL;

        pbyIVHead = (u8 *)(pbyTxBufferAddr + cbHeaderSize + cbMacHdLen + uPadding);
        pbyPayloadHead = (u8 *)(pbyTxBufferAddr + cbHeaderSize + cbMacHdLen + uPadding + cbIVlen);
        do {
	    if (pDevice->op_mode == NL80211_IFTYPE_STATION &&
					pDevice->bLinkPass == true) {
                pbyBSSID = pDevice->abyBSSID;
                // get pairwise key
                if (KeybGetTransmitKey(&(pDevice->sKey), pbyBSSID, PAIRWISE_KEY, &pTransmitKey) == false) {
                    // get group key
                    if(KeybGetTransmitKey(&(pDevice->sKey), pbyBSSID, GROUP_KEY, &pTransmitKey) == true) {
                        DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"Get GTK.\n");
                        break;
                    }
                } else {
                    DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"Get PTK.\n");
                    break;
                }
            }
            // get group key
            pbyBSSID = pDevice->abyBroadcastAddr;
            if(KeybGetTransmitKey(&(pDevice->sKey), pbyBSSID, GROUP_KEY, &pTransmitKey) == false) {
                pTransmitKey = NULL;
		DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"KEY is NULL. OP Mode[%d]\n", pDevice->op_mode);
            } else {
                DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"Get GTK.\n");
            }
        } while(false);
        //Fill TXKEY
	s_vFillTxKey(pContext, pTxBufHead, pbyIVHead, pTransmitKey,
			(u16)cbFrameBodySize, NULL);

        memcpy(pMACHeader, pPacket->p80211Header, cbMacHdLen);
        memcpy(pbyPayloadHead, ((u8 *)(pPacket->p80211Header) + cbMacHdLen),
                 cbFrameBodySize);
    }
    else {
        // Copy the Packet into a tx Buffer
        memcpy(pMACHeader, pPacket->p80211Header, pPacket->cbMPDULen);
    }

    pMACHeader->seq_ctrl = cpu_to_le16(pDevice->wSeqCounter << 4);
    pDevice->wSeqCounter++ ;
    if (pDevice->wSeqCounter > 0x0fff)
        pDevice->wSeqCounter = 0;

    if (bIsPSPOLL) {
        // The MAC will automatically replace the Duration-field of MAC header by Duration-field
        // of FIFO control header.
        // This will cause AID-field of PS-POLL packet be incorrect (Because PS-POLL's AID field is
        // in the same place of other packet's Duration-field).
        // And it will cause Cisco-AP to issue Disassociation-packet
	if (byPktType == PK_TYPE_11GB || byPktType == PK_TYPE_11GA) {
		struct vnt_tx_datahead_g *data_head = &pTX_Buffer->tx_head.
						tx_cts.tx.head.cts_g.data_head;
		data_head->duration_a =
			cpu_to_le16(pPacket->p80211Header->sA2.wDurationID);
		data_head->duration_b =
			cpu_to_le16(pPacket->p80211Header->sA2.wDurationID);
	} else {
		struct vnt_tx_datahead_ab *data_head = &pTX_Buffer->tx_head.
					tx_ab.tx.head.data_head_ab;
		data_head->duration =
			cpu_to_le16(pPacket->p80211Header->sA2.wDurationID);
	}
    }

    pTX_Buffer->tx_byte_count = cpu_to_le16((u16)(cbReqCount));
    pTX_Buffer->byPKTNO = (u8) (((wCurrentRate<<4) &0x00F0) | ((pDevice->wSeqCounter - 1) & 0x000F));
    pTX_Buffer->byType = 0x00;

	pContext->skb = NULL;
	pContext->type = CONTEXT_MGMT_PACKET;
	pContext->buf_len = (u16)cbReqCount + 4; /* USB header */

    if (WLAN_GET_FC_TODS(pMACHeader->frame_control) == 0) {
	s_vSaveTxPktInfo(pDevice, (u8)(pTX_Buffer->byPKTNO & 0x0F),
			&pMACHeader->addr1[0], (u16)cbFrameSize,
			pTxBufHead->wFIFOCtl);
    }
    else {
	s_vSaveTxPktInfo(pDevice, (u8)(pTX_Buffer->byPKTNO & 0x0F),
			&pMACHeader->addr3[0], (u16)cbFrameSize,
			pTxBufHead->wFIFOCtl);
    }

    PIPEnsSendBulkOut(pDevice,pContext);

	spin_unlock_irqrestore(&pDevice->lock, flags);

    return CMD_STATUS_PENDING;
}

CMD_STATUS csBeacon_xmit(struct vnt_private *pDevice,
	struct vnt_tx_mgmt *pPacket)
{
	struct vnt_beacon_buffer *pTX_Buffer;
	struct vnt_tx_short_buf_head *short_head;
	u32 cbFrameSize = pPacket->cbMPDULen + WLAN_FCS_LEN;
	u32 cbHeaderSize = 0;
	struct ieee80211_hdr *pMACHeader;
	u16 wCurrentRate;
	u32 cbFrameBodySize;
	u32 cbReqCount;
	struct vnt_usb_send_context *pContext;
	CMD_STATUS status;

	pContext = s_vGetFreeContext(pDevice);
    if (NULL == pContext) {
        status = CMD_STATUS_RESOURCES;
        DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"ManagementSend TX...NO CONTEXT!\n");
        return status ;
    }

	pTX_Buffer = (struct vnt_beacon_buffer *)&pContext->data[0];
	short_head = &pTX_Buffer->short_head;

    cbFrameBodySize = pPacket->cbPayloadLen;

	cbHeaderSize = sizeof(struct vnt_tx_short_buf_head);

	if (pDevice->byBBType == BB_TYPE_11A) {
		wCurrentRate = RATE_6M;

		/* Get SignalField,ServiceField,Length */
		vnt_get_phy_field(pDevice, cbFrameSize, wCurrentRate,
			PK_TYPE_11A, &short_head->ab);

		/* Get Duration and TimeStampOff */
		short_head->duration = s_uGetDataDuration(pDevice,
							PK_TYPE_11A, false);
		short_head->time_stamp_off =
				vnt_time_stamp_off(pDevice, wCurrentRate);
	} else {
		wCurrentRate = RATE_1M;
		short_head->fifo_ctl |= FIFOCTL_11B;

		/* Get SignalField,ServiceField,Length */
		vnt_get_phy_field(pDevice, cbFrameSize, wCurrentRate,
					PK_TYPE_11B, &short_head->ab);

		/* Get Duration and TimeStampOff */
		short_head->duration = s_uGetDataDuration(pDevice,
						PK_TYPE_11B, false);
		short_head->time_stamp_off =
			vnt_time_stamp_off(pDevice, wCurrentRate);
	}


	/* Generate Beacon Header */
	pMACHeader = &pTX_Buffer->hdr;

	memcpy(pMACHeader, pPacket->p80211Header, pPacket->cbMPDULen);

	pMACHeader->duration_id = 0;
	pMACHeader->seq_ctrl = cpu_to_le16(pDevice->wSeqCounter << 4);
	pDevice->wSeqCounter++;
	if (pDevice->wSeqCounter > 0x0fff)
		pDevice->wSeqCounter = 0;

    cbReqCount = cbHeaderSize + WLAN_HDR_ADDR3_LEN + cbFrameBodySize;

    pTX_Buffer->tx_byte_count = cpu_to_le16((u16)cbReqCount);
    pTX_Buffer->byPKTNO = (u8) (((wCurrentRate<<4) &0x00F0) | ((pDevice->wSeqCounter - 1) & 0x000F));
    pTX_Buffer->byType = 0x01;

	pContext->skb = NULL;
	pContext->type = CONTEXT_MGMT_PACKET;
	pContext->buf_len = (u16)cbReqCount + 4; /* USB header */

    PIPEnsSendBulkOut(pDevice,pContext);
    return CMD_STATUS_PENDING;

}

//TYPE_AC0DMA data tx
/*
 * Description:
 *      Tx packet via AC0DMA(DMA1)
 *
 * Parameters:
 *  In:
 *      pDevice         - Pointer to the adapter
 *      skb             - Pointer to tx skb packet
 *  Out:
 *      void
 *
 * Return Value: NULL
 */

int nsDMA_tx_packet(struct vnt_private *pDevice, struct sk_buff *skb)
{
	struct net_device_stats *pStats = &pDevice->stats;
	struct vnt_manager *pMgmt = &pDevice->vnt_mgmt;
	struct vnt_tx_buffer *pTX_Buffer;
	u32 BytesToWrite = 0, uHeaderLen = 0;
	u32 uNodeIndex = 0;
	u8 byMask[8] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80};
	u16 wAID;
	u8 byPktType;
	int bNeedEncryption = false;
	PSKeyItem pTransmitKey = NULL;
	int ii;
	int bTKIP_UseGTK = false;
	int bNeedDeAuth = false;
	u8 *pbyBSSID;
	int bNodeExist = false;
	struct vnt_usb_send_context *pContext;
	bool fConvertedPacket;
	u32 status;
	u16 wKeepRate = pDevice->wCurrentRate;
	int bTxeapol_key = false;

    if (pMgmt->eCurrMode == WMAC_MODE_ESS_AP) {

        if (pDevice->uAssocCount == 0) {
            dev_kfree_skb_irq(skb);
            return 0;
        }

	if (is_multicast_ether_addr((u8 *)(skb->data))) {
            uNodeIndex = 0;
            bNodeExist = true;
            if (pMgmt->sNodeDBTable[0].bPSEnable) {

                skb_queue_tail(&(pMgmt->sNodeDBTable[0].sTxPSQueue), skb);
                pMgmt->sNodeDBTable[0].wEnQueueCnt++;
                // set tx map
                pMgmt->abyPSTxMap[0] |= byMask[0];
                return 0;
            }
            // multicast/broadcast data rate

            if (pDevice->byBBType != BB_TYPE_11A)
                pDevice->wCurrentRate = RATE_2M;
            else
                pDevice->wCurrentRate = RATE_24M;
            // long preamble type
            pDevice->byPreambleType = PREAMBLE_SHORT;

        }else {

            if (BSSbIsSTAInNodeDB(pDevice, (u8 *)(skb->data), &uNodeIndex)) {

                if (pMgmt->sNodeDBTable[uNodeIndex].bPSEnable) {

                    skb_queue_tail(&pMgmt->sNodeDBTable[uNodeIndex].sTxPSQueue, skb);

                    pMgmt->sNodeDBTable[uNodeIndex].wEnQueueCnt++;
                    // set tx map
                    wAID = pMgmt->sNodeDBTable[uNodeIndex].wAID;
                    pMgmt->abyPSTxMap[wAID >> 3] |=  byMask[wAID & 7];
                    DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO "Set:pMgmt->abyPSTxMap[%d]= %d\n",
                             (wAID >> 3), pMgmt->abyPSTxMap[wAID >> 3]);

                    return 0;
                }
                // AP rate decided from node
                pDevice->wCurrentRate = pMgmt->sNodeDBTable[uNodeIndex].wTxDataRate;
                // tx preamble decided from node

                if (pMgmt->sNodeDBTable[uNodeIndex].bShortPreamble) {
                    pDevice->byPreambleType = pDevice->byShortPreamble;

                }else {
                    pDevice->byPreambleType = PREAMBLE_LONG;
                }
                bNodeExist = true;
            }
        }

        if (bNodeExist == false) {
            DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG"Unknown STA not found in node DB \n");
            dev_kfree_skb_irq(skb);
            return 0;
        }
    }

	memcpy(&pDevice->sTxEthHeader, skb->data, ETH_HLEN);

//mike add:station mode check eapol-key challenge--->
{
    u8  Protocol_Version;    //802.1x Authentication
    u8  Packet_Type;           //802.1x Authentication
    u8  Descriptor_type;
    u16 Key_info;

    Protocol_Version = skb->data[ETH_HLEN];
    Packet_Type = skb->data[ETH_HLEN+1];
    Descriptor_type = skb->data[ETH_HLEN+1+1+2];
    Key_info = (skb->data[ETH_HLEN+1+1+2+1] << 8)|(skb->data[ETH_HLEN+1+1+2+2]);
	if (pDevice->sTxEthHeader.h_proto == cpu_to_be16(ETH_P_PAE)) {
		/* 802.1x OR eapol-key challenge frame transfer */
		if (((Protocol_Version == 1) || (Protocol_Version == 2)) &&
			(Packet_Type == 3)) {
                        bTxeapol_key = true;
                       if(!(Key_info & BIT3) &&  //WPA or RSN group-key challenge
			   (Key_info & BIT8) && (Key_info & BIT9)) {    //send 2/2 key
			  if(Descriptor_type==254) {
                               pDevice->fWPA_Authened = true;
			     PRINT_K("WPA ");
			  }
			  else {
                               pDevice->fWPA_Authened = true;
			     PRINT_K("WPA2(re-keying) ");
			  }
			  PRINT_K("Authentication completed!!\n");
                        }
		    else if((Key_info & BIT3) && (Descriptor_type==2) &&  //RSN pairwise-key challenge
			       (Key_info & BIT8) && (Key_info & BIT9)) {
			  pDevice->fWPA_Authened = true;
                            PRINT_K("WPA2 Authentication completed!!\n");
		     }
             }
   }
}
//mike add:station mode check eapol-key challenge<---

    if (pDevice->bEncryptionEnable == true) {
        bNeedEncryption = true;
        // get Transmit key
        do {
            if ((pMgmt->eCurrMode == WMAC_MODE_ESS_STA) &&
                (pMgmt->eCurrState == WMAC_STATE_ASSOC)) {
                pbyBSSID = pDevice->abyBSSID;
                // get pairwise key
                if (KeybGetTransmitKey(&(pDevice->sKey), pbyBSSID, PAIRWISE_KEY, &pTransmitKey) == false) {
                    // get group key
                    if(KeybGetTransmitKey(&(pDevice->sKey), pbyBSSID, GROUP_KEY, &pTransmitKey) == true) {
                        bTKIP_UseGTK = true;
                        DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG"Get GTK.\n");
                        break;
                    }
                } else {
                    DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG"Get PTK.\n");
                    break;
                }
            }else if (pMgmt->eCurrMode == WMAC_MODE_IBSS_STA) {
	      /* TO_DS = 0 and FROM_DS = 0 --> 802.11 MAC Address1 */
                pbyBSSID = pDevice->sTxEthHeader.h_dest;
                DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG"IBSS Serach Key: \n");
                for (ii = 0; ii< 6; ii++)
                    DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG"%x \n", *(pbyBSSID+ii));
                DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG"\n");

                // get pairwise key
                if(KeybGetTransmitKey(&(pDevice->sKey), pbyBSSID, PAIRWISE_KEY, &pTransmitKey) == true)
                    break;
            }
            // get group key
            pbyBSSID = pDevice->abyBroadcastAddr;
            if(KeybGetTransmitKey(&(pDevice->sKey), pbyBSSID, GROUP_KEY, &pTransmitKey) == false) {
                pTransmitKey = NULL;
                if (pMgmt->eCurrMode == WMAC_MODE_IBSS_STA) {
                    DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG"IBSS and KEY is NULL. [%d]\n", pMgmt->eCurrMode);
                }
                else
                    DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG"NOT IBSS and KEY is NULL. [%d]\n", pMgmt->eCurrMode);
            } else {
                bTKIP_UseGTK = true;
                DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG"Get GTK.\n");
            }
        } while(false);
    }

    byPktType = (u8)pDevice->byPacketType;

    if (pDevice->bFixRate) {
        if (pDevice->byBBType == BB_TYPE_11B) {
            if (pDevice->uConnectionRate >= RATE_11M) {
                pDevice->wCurrentRate = RATE_11M;
            } else {
                pDevice->wCurrentRate = (u16)pDevice->uConnectionRate;
            }
        } else {
            if ((pDevice->byBBType == BB_TYPE_11A) &&
                (pDevice->uConnectionRate <= RATE_6M)) {
                pDevice->wCurrentRate = RATE_6M;
            } else {
                if (pDevice->uConnectionRate >= RATE_54M)
                    pDevice->wCurrentRate = RATE_54M;
                else
                    pDevice->wCurrentRate = (u16)pDevice->uConnectionRate;
            }
        }
    }
    else {
	if (pDevice->op_mode == NL80211_IFTYPE_ADHOC) {
            // Adhoc Tx rate decided from node DB
	    if (is_multicast_ether_addr(pDevice->sTxEthHeader.h_dest)) {
                // Multicast use highest data rate
                pDevice->wCurrentRate = pMgmt->sNodeDBTable[0].wTxDataRate;
                // preamble type
                pDevice->byPreambleType = pDevice->byShortPreamble;
            }
            else {
                if (BSSbIsSTAInNodeDB(pDevice, &(pDevice->sTxEthHeader.h_dest[0]), &uNodeIndex)) {
                    pDevice->wCurrentRate = pMgmt->sNodeDBTable[uNodeIndex].wTxDataRate;
                    if (pMgmt->sNodeDBTable[uNodeIndex].bShortPreamble) {
                        pDevice->byPreambleType = pDevice->byShortPreamble;

                    }
                    else {
                        pDevice->byPreambleType = PREAMBLE_LONG;
                    }
                    DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"Found Node Index is [%d]  Tx Data Rate:[%d]\n",uNodeIndex, pDevice->wCurrentRate);
                }
                else {
                    if (pDevice->byBBType != BB_TYPE_11A)
                       pDevice->wCurrentRate = RATE_2M;
                    else
                       pDevice->wCurrentRate = RATE_24M; // refer to vMgrCreateOwnIBSS()'s
                                                         // abyCurrExtSuppRates[]
                    pDevice->byPreambleType = PREAMBLE_SHORT;
                    DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"Not Found Node use highest basic Rate.....\n");
                }
            }
        }
	if (pDevice->op_mode == NL80211_IFTYPE_STATION) {
            // Infra STA rate decided from AP Node, index = 0
            pDevice->wCurrentRate = pMgmt->sNodeDBTable[0].wTxDataRate;
        }
    }

	if (pDevice->sTxEthHeader.h_proto == cpu_to_be16(ETH_P_PAE)) {
		if (pDevice->byBBType != BB_TYPE_11A) {
			pDevice->wCurrentRate = RATE_1M;
			pDevice->byTopCCKBasicRate = RATE_1M;
			pDevice->byTopOFDMBasicRate = RATE_6M;
		} else {
			pDevice->wCurrentRate = RATE_6M;
			pDevice->byTopCCKBasicRate = RATE_1M;
			pDevice->byTopOFDMBasicRate = RATE_6M;
		}
	}

    DBG_PRT(MSG_LEVEL_DEBUG,
	    KERN_INFO "dma_tx: pDevice->wCurrentRate = %d\n",
	    pDevice->wCurrentRate);

    if (wKeepRate != pDevice->wCurrentRate) {
	bScheduleCommand((void *) pDevice, WLAN_CMD_SETPOWER, NULL);
    }

    if (pDevice->wCurrentRate <= RATE_11M) {
        byPktType = PK_TYPE_11B;
    }

    if (bNeedEncryption == true) {
        DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"ntohs Pkt Type=%04x\n", ntohs(pDevice->sTxEthHeader.h_proto));
	if ((pDevice->sTxEthHeader.h_proto) == cpu_to_be16(ETH_P_PAE)) {
		bNeedEncryption = false;
            DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"Pkt Type=%04x\n", (pDevice->sTxEthHeader.h_proto));
            if ((pMgmt->eCurrMode == WMAC_MODE_ESS_STA) && (pMgmt->eCurrState == WMAC_STATE_ASSOC)) {
                if (pTransmitKey == NULL) {
                    DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"Don't Find TX KEY\n");
                }
                else {
                    if (bTKIP_UseGTK == true) {
                        DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"error: KEY is GTK!!~~\n");
                    }
                    else {
			DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"Find PTK [%X]\n",
				pTransmitKey->dwKeyIndex);
                        bNeedEncryption = true;
                    }
                }
            }
        }
        else {

            if (pTransmitKey == NULL) {
                DBG_PRT(MSG_LEVEL_DEBUG, KERN_INFO"return no tx key\n");
                dev_kfree_skb_irq(skb);
                pStats->tx_dropped++;
                return STATUS_FAILURE;
            }
        }
    }

	pContext = s_vGetFreeContext(pDevice);
	if (!pContext) {
		DBG_PRT(MSG_LEVEL_DEBUG, KERN_DEBUG" pContext == NULL\n");
		dev_kfree_skb_irq(skb);
		return STATUS_RESOURCES;
	}

	pTX_Buffer = (struct vnt_tx_buffer *)&pContext->data[0];

	fConvertedPacket = s_bPacketToWirelessUsb(pContext, byPktType,
			pTX_Buffer, bNeedEncryption,
			skb->len, &pDevice->sTxEthHeader,
                        (u8 *)skb->data, pTransmitKey, uNodeIndex,
                        pDevice->wCurrentRate,
                        &uHeaderLen, &BytesToWrite
                       );

	if (fConvertedPacket == false) {
		pContext->in_use = false;
		dev_kfree_skb_irq(skb);
		return STATUS_FAILURE;
	}

    if ( pDevice->bEnablePSMode == true ) {
        if ( !pDevice->bPSModeTxBurst ) {
		bScheduleCommand((void *) pDevice,
				 WLAN_CMD_MAC_DISPOWERSAVING,
				 NULL);
            pDevice->bPSModeTxBurst = true;
        }
    }

    pTX_Buffer->byPKTNO = (u8) (((pDevice->wCurrentRate<<4) &0x00F0) | ((pDevice->wSeqCounter - 1) & 0x000F));
    pTX_Buffer->tx_byte_count = cpu_to_le16((u16)BytesToWrite);

	pContext->skb = skb;
	pContext->type = CONTEXT_DATA_PACKET;
	pContext->buf_len = (u16)BytesToWrite + 4 ; /* USB header */

    s_vSaveTxPktInfo(pDevice, (u8)(pTX_Buffer->byPKTNO & 0x0F),
			&pDevice->sTxEthHeader.h_dest[0],
			(u16)(BytesToWrite-uHeaderLen),
			pTX_Buffer->fifo_head.wFIFOCtl);

    status = PIPEnsSendBulkOut(pDevice,pContext);

    if (bNeedDeAuth == true) {
        u16 wReason = WLAN_MGMT_REASON_MIC_FAILURE;

	bScheduleCommand((void *) pDevice, WLAN_CMD_DEAUTH, (u8 *) &wReason);
    }

	if (status != STATUS_PENDING) {
		pContext->in_use = false;
		dev_kfree_skb_irq(skb);
		return STATUS_FAILURE;
	}


	return 0;
}

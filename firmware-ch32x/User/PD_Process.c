/**** (C) COPYRIGHT ****
* File Name          : PD_process.c
* Description        : Merged Source+Sink PD process with PB3 mux select and PB11 host-detect.
*
* - SOURCE: advertises 4 PDOs (5.1V@5A, 9V@3A, 12V@2.25A, 15V@1.8A) + your TPS55289 control
* - SINK: on PB11 falling edge enters sink for up to 500ms, requests 12V PDO if available
* - Flag: PD_ProfileAccepted is set when SINK receives PS_RDY (successful contract)
*
* 2026-01-29: Added SNK-success lockout. After successful SNK contract, re-entry to SNK is blocked
*            until PB11 has been HIGH continuously for PB11_REARM_HIGH_MS.
****/

#include "debug.h"
#include <string.h>
#include "PD_Process.h"
#include "tps55289.h"

void USBPD_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/* keep your peripheral hooks unchanged */
extern void VBUS_enable(void);
extern void VBUS_disable(void);
extern void VBUS_set_5V(void);

/* ==== Board signals ====
 * PB3  : PDC_CC_SEL (0 => route CC to SOURCE receptacle, 1 => route CC to SINK receptacle)
 * PB11 : PDC_CC_DET (active-low host detect)
 */
#define PDC_CC_SEL_PORT   GPIOB
#define PDC_CC_SEL_PIN    GPIO_Pin_3
#define PDC_CC_DET_PORT   GPIOB
#define PDC_CC_DET_PIN    GPIO_Pin_11

/* SNK window */
#define SNK_WINDOW_MS             500
#define PB11_RELEASE_DEBOUNCE_MS  100   /* set e.g. 20..50 if needed */

/* After successful SNK contract, require PB11=1 continuously before allowing another falling edge */
#define PB11_REARM_HIGH_MS        1000 /* 2000..5000ms */

typedef enum { ROLE_SNK = 0, ROLE_SRC = 1 } pd_role_t;
static volatile pd_role_t g_role = ROLE_SRC;
static UINT16 g_snk_window_ms = 0;
static UINT8  g_pb11_prev = 1;
static UINT16 g_pb11_release_ms = 0;

/* lockout flag: blocks re-entering SNK after successful SNK contract */
static volatile UINT8 g_snk_success_lockout = 0;
static UINT16 g_pb11_high_stable_ms = 0;

/* profile accepted flag (for application) */
static volatile UINT8 g_pd_profile_accepted = 0;

/* Cached sink negotiation parameters */
static UINT16 g_snk_req_voltage_mV = 0;
static UINT16 g_snk_req_current_mA = 0;
static UINT16 g_snk_neg_voltage_100mV = 0;
static UINT16 g_snk_neg_current_100mA = 0;

UINT8 PD_ProfileAccepted_Get(void) { return g_pd_profile_accepted; }
UINT8 PD_ProfileAccepted_Consume(void)
{
    UINT8 v = g_pd_profile_accepted;
    g_pd_profile_accepted = 0;
    return v;
}

static inline UINT8 PDC_Read_PB11_Level(void)
{
    return (GPIO_ReadInputDataBit(PDC_CC_DET_PORT, PDC_CC_DET_PIN) == Bit_SET) ? 1 : 0;
}
static inline UINT8 PDC_HostDetected_ActiveLow(void)
{
    return (GPIO_ReadInputDataBit(PDC_CC_DET_PORT, PDC_CC_DET_PIN) == Bit_RESET);
}
static inline void PDC_SetCcMuxToSink(UINT8 en)
{
    GPIO_WriteBit(PDC_CC_SEL_PORT, PDC_CC_SEL_PIN, en ? Bit_SET : Bit_RESET);
}

/* PD buffers */
__attribute__ ((aligned(4))) uint8_t PD_Rx_Buf[ 34 ];                    /* PD receive buffer */
__attribute__ ((aligned(4))) uint8_t PD_Tx_Buf[ 34 ];                    /* PD send buffer */

/****/
UINT8 PD_Ack_Buf[ 2 ];                    /* PD-ACK buffer */

UINT8  Tmr_Ms_Cnt_Last;                    /* System timer millisecond timing final value */
UINT8  Tmr_Ms_Dlt;                    /* System timer millisecond timing this interval value */
PD_CONTROL PD_Ctl;                    /* PD Control Related Structures */

UINT8  Adapter_SrcCap[ 30 ];                    /* Contents of the SrcCap message for the adapter */
UINT8  PDO_Len;

UINT8 SinkReqPDOIndex;

UINT8 PD_Get_Role(void) { return (UINT8)g_role; }
UINT8 PD_Get_SinkReqPDOIndex(void) { return SinkReqPDOIndex; }
UINT16 PD_Get_Snk_Voltage_100mV(void) { return g_snk_neg_voltage_100mV; }
UINT16 PD_Get_Snk_Current_100mA(void) { return g_snk_neg_current_100mA; }

/* ==== SOURCE: Raspberry Pi compatible fixed PDO set (5.1/9/12/15) ==== */
/* SrcCap Table */
//PDO profile compatible with Raspberry Pi 5 (flags, 5V@5A, 9V@3A, 12V@2.25A, 15V@1.8A)
UINT8 SrcCap_5V5A_Tab[ 16 ] =
{
    0xF4, 0x91, 0x01, 0x0A,   /* 5V 5A   (PDO #1, dual-role + ext-power flags) */
    0x2C, 0xD1, 0x02, 0x00,   /* 9V 3A */
    0xE1, 0xC0, 0x03, 0x00,   /* 12V 2.25A */
    0xB4, 0xB0, 0x04, 0x00,   /* 15V 1.8A */
};

/* PD3.0 */
UINT8 SrcCap_Ext_Tab[ 28 ] =
{
    0X18, 0X80, 0X63, 0X00,
    0X00, 0X00, 0X00, 0X00,
    0X00, 0X00, 0X01, 0X00,
    0X00, 0X00, 0X07, 0X03,
    0X00, 0X00, 0X00, 0X00,
    0X00, 0X00, 0X00, 0X03,
    0X00, 0X12, 0X00, 0X00,
};

UINT8 Status_Ext_Tab[ 8 ] =
{
    0X06, 0X80, 0X16, 0X00,
    0X00, 0X00, 0X00, 0X00,
};

/* VDM Source reply Table - Raspberry Pi 27W Power Supply Identifier */
UINT8 VDM_RPI_TX_Tab[ 20 ] =
{
    0x41, 0xA0, 0x00, 0xFF,
    0x8A, 0x2E, 0xC0, 0x01,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0x00,
    0x01, 0x00, 0x80, 0x20,
};

/* ==== SINK side: minimal sink capability table (kept from WCH sample) ==== */
UINT8 SrcCap_5V3A_Tab[ 4 ]  = { 0X2C, 0X91, 0X01, 0X3E };
UINT8 SrcCap_5V2A_Tab[ 4 ]  = { 0XC8, 0X90, 0X01, 0X3E };
UINT8 SinkCap_5V1A_Tab[ 4 ] = { 0X64, 0X90, 0X01, 0X36 };

/* optional sink status (from your sink.c); keep symbol if your project uses it */
volatile UINT8 PD_Sink_Stat = 0x00;

/* role-dependent GOODCRC header[0] */
static inline UINT8 PD_GoodCRC_Hdr0(void)
{
    return (g_role == ROLE_SRC) ? 0x61 : 0x41;
}

/****
 * @fn      USBPD_IRQHandler
 *
 * @brief   This function handles USBPD interrupt.
 *
 * @return  none
 */
void USBPD_IRQHandler(void)
{
    if(USBPD->STATUS & IF_RX_ACT)
    {
        USBPD->STATUS |= IF_RX_ACT;
        if( ( USBPD->STATUS & MASK_PD_STAT ) == PD_RX_SOP0 )
        {
            if( USBPD->BMC_BYTE_CNT >= 6 )
            {
                /* If GOODCRC, do not answer and ignore this reception */
                if( ( USBPD->BMC_BYTE_CNT != 6 ) || ( ( PD_Rx_Buf[ 0 ] & 0x1F ) != DEF_TYPE_GOODCRC ) )
                {
                    Delay_Us(30);                    /* Delay 30us, answer GoodCRC */
                    PD_Ack_Buf[ 0 ] = PD_GoodCRC_Hdr0();
                    PD_Ack_Buf[ 1 ] = ( PD_Rx_Buf[ 1 ] & 0x0E ) | PD_Ctl.Flag.Bit.Auto_Ack_PRRole;
                    USBPD->CONFIG |= IE_TX_END ;
                    PD_Phy_SendPack( 0, PD_Ack_Buf, 2, UPD_SOP0 );
                }
            }
        }
    }

    if(USBPD->STATUS & IF_TX_END)
    {
        /* Packet send completion interrupt (GoodCRC send completion interrupt only) */
        USBPD->PORT_CC1 &= ~CC_LVE;
        USBPD->PORT_CC2 &= ~CC_LVE;

        /* Interrupts are turned off and can be turned on after the main function has finished processing the data */
        NVIC_DisableIRQ(USBPD_IRQn);
        PD_Ctl.Flag.Bit.Msg_Recvd = 1;                    /* Packet received flag */
        USBPD->STATUS |= IF_TX_END;
    }

    if(USBPD->STATUS & IF_RX_RESET)
    {
        USBPD->STATUS |= IF_RX_RESET;
        /* re-init based on current role */
        if(g_role == ROLE_SRC) PD_SRC_Init();
        else                   PD_SINK_Init();
        printf("IF_RX_RESET\r\n");
    }
}

/****
 * @fn      PD_Rx_Mode
 *
 * @brief   This function uses to enter reception mode.
 *
 * @return  none
 */
void PD_Rx_Mode( void )
{
    USBPD->CONFIG |= PD_ALL_CLR;
    USBPD->CONFIG &= ~PD_ALL_CLR;
    USBPD->CONFIG |= IE_RX_ACT | IE_RX_RESET | PD_DMA_EN;
    USBPD->DMA = (UINT32)(UINT8 *)PD_Rx_Buf;
    USBPD->CONTROL &= ~PD_TX_EN;
    USBPD->BMC_CLK_CNT = UPD_TMR_RX_48M;
    USBPD->CONTROL |= BMC_START;
    NVIC_EnableIRQ( USBPD_IRQn );
}

/****
 * @fn      PD_SRC_Init
 *
 * @brief   This function uses to initialize SRC mode.
 *
 * @return  none
 */
void PD_SRC_Init( void )
{
    PD_Ctl.Flag.Bit.PR_Role = 1;                    /* SRC mode */
    PD_Ctl.Flag.Bit.Auto_Ack_PRRole = 1;                    /* Default auto-responder role is SRC */
    USBPD->PORT_CC1 = CC_CMP_66 | CC_PU_330;
    USBPD->PORT_CC2 = CC_CMP_66 | CC_PU_330;
    g_role = ROLE_SRC;
    GPIO_WriteBit(GPIOB, GPIO_Pin_12, 0);
}

/****
 * @fn      PD_SINK_Init
 *
 * @brief   This function uses to initialize SNK mode.
 *
 * @return  none
 */
void PD_SINK_Init( void )
{
    PD_Ctl.Flag.Bit.PR_Role = 0;                    /* SINK mode */
    PD_Ctl.Flag.Bit.Auto_Ack_PRRole = 0;                    /* Default auto-responder role is SINK */
    USBPD->PORT_CC1 = CC_CMP_66 | CC_PD;
    USBPD->PORT_CC2 = CC_CMP_66 | CC_PD;
    g_role = ROLE_SNK;
    GPIO_WriteBit(GPIOB, GPIO_Pin_12, 1);
}

/* ==== PHY reset (role-aware) ==== */
/****
 * @fn      PD_PHY_Reset_Role
 *
 * @brief   This function uses to reset PD PHY (role-aware).
 *
 * @return  none
 */
static void PD_PHY_Reset_Role(pd_role_t role)
{
    /* clear app flag on every role reset (you can change if you want latched) */
    g_pd_profile_accepted = 0;

    /* Initialize all variables (based on WCH sample style) */
    PD_Ctl.Flag.Bit.Msg_Recvd = 0;
    PD_Ctl.Msg_ID = 0;
    PD_Ctl.Flag.Bit.PD_Version = 1;
    PD_Ctl.Det_Cnt = 0;
    PD_Ctl.Flag.Bit.Connected = 0;
    PD_Ctl.PD_Comm_Timer = 0;
    PD_Ctl.PD_BusIdle_Timer = 0;
    PD_Ctl.Mode_Try_Cnt = 0x80;
    PD_Ctl.Flag.Bit.Stop_Det_Chk = 0;
    PD_Ctl.PD_State = STA_IDLE;
    PD_Ctl.Flag.Bit.PD_Comm_Succ = 0;

	if(role == ROLE_SRC) {
		PD_Ctl.Flag.Bit.PD_Role = 1; // DFP
		PD_SRC_Init();
	} else {
		PD_Ctl.Flag.Bit.PD_Role = 0; // UFP
		PD_SINK_Init();
	}

    /* sink extra */
    PD_Sink_Stat = 0x00;

    if(role == ROLE_SRC) PD_SRC_Init();
    else                 PD_SINK_Init();

    PD_Rx_Mode();
}


/* ==== role manager tick (call from main loop) ==== */
/****
 * @fn      PD_Role_Manager_Tick
 *
 * @brief   Role switch helper (PB11 host-detect + PB3 CC mux select).
 *
 * @return  none
 */
void PD_Role_Manager_Tick(void)
{
    UINT8 pb11_now = PDC_Read_PB11_Level();

    /* Clear lockout only after PB11 has been HIGH continuously for PB11_REARM_HIGH_MS */

    if(g_snk_success_lockout)
    {
        if(pb11_now)
        {
            if(g_pb11_high_stable_ms < PB11_REARM_HIGH_MS)
            {
				g_pb11_high_stable_ms += Tmr_Ms_Dlt;
	            if(g_pb11_high_stable_ms >= PB11_REARM_HIGH_MS)
	            {
	                g_snk_success_lockout = 0;
	                g_pb11_prev = pb11_now; // avoid synthetic falling immediately after unlock
	            }
			}
        }
        else
        {
            g_pb11_high_stable_ms = 0;
        }
        return;
    }

    UINT8 falling = (g_pb11_prev == 1) && (pb11_now == 0);
    g_pb11_prev = pb11_now;

    if(g_role == ROLE_SRC)
    {
        if(falling && (g_snk_success_lockout == 0))
        {
            /* enter SNK window */
            g_pd_profile_accepted = 0;
            PDC_SetCcMuxToSink(1);
            PD_PHY_Reset_Role(ROLE_SNK);
            g_snk_window_ms = SNK_WINDOW_MS;
            g_pb11_release_ms = 0;
            g_snk_success_lockout = 1;
        }
    }
    else
    {
        // host removed? (PB11 back to 1) => immediate return (optional debounce)
        if(!PDC_HostDetected_ActiveLow())
        {
            if(PB11_RELEASE_DEBOUNCE_MS == 0)
            {
                PDC_SetCcMuxToSink(0);
                PD_PHY_Reset_Role(ROLE_SRC);
                g_snk_window_ms = 0;
                return;
            }
            else
            {
                if(g_pb11_release_ms < PB11_RELEASE_DEBOUNCE_MS)
                    g_pb11_release_ms += Tmr_Ms_Dlt;

                if(g_pb11_release_ms >= PB11_RELEASE_DEBOUNCE_MS)
                {
                    PDC_SetCcMuxToSink(0);
                    PD_PHY_Reset_Role(ROLE_SRC);
                    g_snk_window_ms = 0;
                    return;
                }
            }
        }
        else
        {
            g_pb11_release_ms = 0;
        }

        if(g_snk_window_ms > 0)
        {
            if(g_snk_window_ms > Tmr_Ms_Dlt) g_snk_window_ms -= Tmr_Ms_Dlt;
            else g_snk_window_ms = 0;

            if(g_snk_window_ms == 0)
            {
                PDC_SetCcMuxToSink(0);
                PD_PHY_Reset_Role(ROLE_SRC);
            }
        }
    }
}

//=====================================================================================================================================================

/*********************************************************************
 * @fn      PD_PHY_Reset
 *
 * @brief   This function uses to reset PD PHY.
 *
 * @return  none
 */
void PD_PHY_Reset( void )
{
    PD_PHY_Reset_Role(g_role);
}

/*********************************************************************
 * @fn      PD_Init
 *
 * @brief   This function uses to initialize PD registers and states.
 *
 * @return  none
 */
void PD_Init( void )
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);               /* Open PD I/O clock, AFIO clock and PD clock */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    AFIO->CTLR |= USBPD_IN_HVT | USBPD_PHY_V33;

    USBPD->CONFIG = PD_DMA_EN;
    USBPD->STATUS = BUF_ERR | IF_RX_BIT | IF_RX_BYTE | IF_RX_ACT | IF_RX_RESET | IF_TX_END;

    /* Initialize all variables */
    memset( &PD_Ctl.PD_State, 0x00, sizeof( PD_CONTROL ) );

    /* default Source caps */
    Adapter_SrcCap[ 0 ] = 1;
    memcpy( &Adapter_SrcCap[ 1 ], SrcCap_5V5A_Tab, 16 );

    /* default role = SRC, mux = SRC */
    PDC_SetCcMuxToSink(0);
    PD_PHY_Reset_Role(ROLE_SRC);
    PD_Rx_Mode();
}

/*********************************************************************
 /****
  * @fn      PD_Detect
  *
  * @brief   This function uses to detect CC connection.
  *
  * @return  0:No connection; 1:CC1 connection; 2:CC2 connection
  */
 UINT8 PD_Detect( void )
 {
 	UINT8  ret = 0;
 	UINT8  cmp_cc1 = 0;
 	UINT8  cmp_cc2 = 0;

     if(g_role == ROLE_SRC)
     {
         if(PD_Ctl.Flag.Bit.Connected)                    /*Detect disconnection*/
         {
             USBPD->PORT_CC1 &= ~( CC_CMP_Mask | PA_CC_AI );
             USBPD->PORT_CC1 |= CC_CMP_22;
             Delay_Us(2);
             if( USBPD->PORT_CC1 & PA_CC_AI ) cmp_cc1 = bCC_CMP_22;
             USBPD->PORT_CC1 &= ~( CC_CMP_Mask | PA_CC_AI );
             USBPD->PORT_CC1 |= CC_CMP_66;

             USBPD->PORT_CC2 &= ~( CC_CMP_Mask | PA_CC_AI );
             USBPD->PORT_CC2 |= CC_CMP_22;
             Delay_Us(2);
             if( USBPD->PORT_CC2 & PA_CC_AI ) cmp_cc2 = bCC_CMP_22;
             USBPD->PORT_CC2 &= ~( CC_CMP_Mask | PA_CC_AI );
             USBPD->PORT_CC2 |= CC_CMP_66;

             if((GPIOC->INDR & PIN_CC1) != (uint32_t)Bit_RESET) cmp_cc1 |= bCC_CMP_220;
             if((GPIOC->INDR & PIN_CC2) != (uint32_t)Bit_RESET) cmp_cc2 |= bCC_CMP_220;

             if( USBPD->PORT_CC1 & CC_PD )
             {
                 /* SRC sample code does not handle SNK */
             }
             else
             {
                 if (USBPD->CONFIG & CC_SEL)
                 {
                     ret = ((cmp_cc2 & bCC_CMP_220) == bCC_CMP_220) ? 0 : 2;
                 }
                 else
                 {
                     ret = ((cmp_cc1 & bCC_CMP_220) == bCC_CMP_220) ? 0 : 1;
                 }
             }
         }
         else                    /*Detect insertion*/
         {
             USBPD->PORT_CC1 &= ~( CC_CMP_Mask|PA_CC_AI );
             USBPD->PORT_CC1 |= CC_CMP_22;
             Delay_Us(2);
             if( USBPD->PORT_CC1 & PA_CC_AI ) cmp_cc1 |= bCC_CMP_22;
             USBPD->PORT_CC1 &= ~( CC_CMP_Mask|PA_CC_AI );
             USBPD->PORT_CC1 |= CC_CMP_66;
             Delay_Us(2);
             if( USBPD->PORT_CC1 & PA_CC_AI ) cmp_cc1 |= bCC_CMP_66;
             if((GPIOC->INDR & PIN_CC1) != (uint32_t)Bit_RESET) cmp_cc1 |= bCC_CMP_220;

             USBPD->PORT_CC2 &= ~( CC_CMP_Mask|PA_CC_AI );
             USBPD->PORT_CC2 |= CC_CMP_22;
             Delay_Us(2);
             if( USBPD->PORT_CC2 & PA_CC_AI ) cmp_cc2 |= bCC_CMP_22;
             USBPD->PORT_CC2 &= ~( CC_CMP_Mask|PA_CC_AI );
             USBPD->PORT_CC2 |= CC_CMP_66;
             Delay_Us(2);
             if( USBPD->PORT_CC2 & PA_CC_AI ) cmp_cc2 |= bCC_CMP_66;
             if((GPIOC->INDR & PIN_CC2) != (uint32_t)Bit_RESET) cmp_cc2 |= bCC_CMP_220;

             if( USBPD->PORT_CC1 & CC_PD )
             {
                /* SRC sample code does not handle SNK */
             }
             else
             {
                 if ((((cmp_cc1 & bCC_CMP_66) == bCC_CMP_66) & ((cmp_cc1 & bCC_CMP_220) == 0x00)) == 1)
                 {
                     if ((((cmp_cc2 & bCC_CMP_22) == bCC_CMP_22) & ((cmp_cc2 & bCC_CMP_66) == 0x00)) == 1) ret = 1;
                     if ((cmp_cc2 & bCC_CMP_220) == bCC_CMP_220) ret = 1;
                 }

                 if ((((cmp_cc2 & bCC_CMP_66) == bCC_CMP_66) & ((cmp_cc2 & bCC_CMP_220) == 0x00)) == 1)
                 {
                     if(ret) ret = 0;
                     else
                     {
                     if ((((cmp_cc1 & bCC_CMP_22) == bCC_CMP_22) && ((cmp_cc1 & bCC_CMP_66) == 0x00)) == 1) ret = 2;
                     if ((cmp_cc1 & bCC_CMP_220) == bCC_CMP_220) ret = 2;
                     }
                 }
             }
         }
         return ret;
     }
     else
     {
         if(PD_Ctl.Flag.Bit.Connected)                    /* Detect disconnection */
         {
 			/* According to the usage scenario of PD SNK, whether
 			 * it is removed or not should be determined by detecting
 			 * the Vbus voltage, this code only shows the detection
 			 * and the subsequent communication flow. */
         }
         else                    /* Detect insertion */
         {
             USBPD->PORT_CC1 &= ~( CC_CMP_Mask|PA_CC_AI );
             USBPD->PORT_CC1 |= CC_CMP_22;
             Delay_Us(2);
             if( USBPD->PORT_CC1 & PA_CC_AI ) cmp_cc1 |= bCC_CMP_22;

             USBPD->PORT_CC2 &= ~( CC_CMP_Mask|PA_CC_AI );
             USBPD->PORT_CC2 |= CC_CMP_22;
             Delay_Us(2);
             if( USBPD->PORT_CC2 & PA_CC_AI ) cmp_cc2 |= bCC_CMP_22;

             if (USBPD->PORT_CC1 & CC_PD)
             {
                 if ((cmp_cc1 & bCC_CMP_22) == bCC_CMP_22) ret = 1;
                 if ((cmp_cc2 & bCC_CMP_22) == bCC_CMP_22)
                 {
                     ret = ret ? 1 : 2;   /* Huawei A to C cable has two pull-up resistors */
                 }
             }
 			else
 			{
 				/* SRC mode insertion detection */
 			}
         }
         return ret;
     }
}

/*********************************************************************
 * @fn      PD_Save_Adapter_SrcCap
 *
 * @brief   This function uses to save the adapter SrcCap information.
 *
 * @return  none
 */
void PD_Save_Adapter_SrcCap( void )
{
    UINT8  i, len;

    /* Calculate the number of NDO's (Number of Data Objects) in the Message Header */
    len = ( ( PD_Rx_Buf[ 1 ] >> 4 ) & 0x07 );

    /* Remove the PPS section */
    for( i = 0; i < len; i++ )
    {
        if( ( PD_Rx_Buf[ 2 + ( i << 2 ) + 3 ] & 0xC0 ) == 0xC0 )
        {
            break;
        }
    }

    PDO_Len = i;

    /* Modify SrcCap information */
       /* BIT[31:30] - Fixed Supply */
       /* BIT29 - Dual-Role Power */
       /* BIT28 - USB Suspend Power */
       /* BIT27 - Unconstrained Power */
       /* BIT26 - USB Communications */
       /* BIT25 - Dual-Role Data */
       /* BIT24 - Unchunked Extended Message Supported */
       /* BIT23 - EPR Mode Capable */
       /* BIT22 - Reserved,shall be set to zero */
       /* BIT[21:20] - Peak Current */
       /* BIT[19:10] - Voltage in 50mV units */
       /* BIT[9:0] - Maximum Current in 10mA units */
    PD_Rx_Buf[ 5 ] = 0x3E;

    /* Save the adapter's SrcCap information */
    PD_Rx_Buf[ 1 ] &= 0x8F;
    PD_Rx_Buf[ 1 ] |= i << 4;
    Adapter_SrcCap[ 0 ] = i;
    memcpy( &Adapter_SrcCap[ 1 ], &PD_Rx_Buf[ 2 ], ( i << 2 ) );
}

/*********************************************************************
 * @fn      PD_PDO_Analyse
 *
 * @brief   This function uses to analyse PDO's voltage and current.
 *
 * @return  none
 */
void PD_PDO_Analyse( UINT8 pdo_idx, UINT8 *srccap, UINT16 *current, UINT16 *voltage )
{
    UINT32 temp32;

    temp32 = srccap[ (  ( pdo_idx - 1 ) << 2 ) + 0 ] +
            ( (UINT32)srccap[ ( ( pdo_idx - 1 ) << 2 ) + 1 ] << 8 ) +
            ( (UINT32)srccap[ ( ( pdo_idx - 1 ) << 2 ) + 2 ] << 16 );

    /* Calculation of current values */
    if( current != NULL )
    {
        *current = ( temp32 & 0x00003FF ) * 10;
    }

    /* Calculation of voltage values */
    if( voltage != NULL )
    {
        temp32 = temp32 >> 10;
        *voltage = ( temp32 & 0x00003FF ) * 50;
    }
}

/****
 * @fn      PD_Request_Analyse
 *
 * @brief   This function uses to analyse PDO's current.
 *
 * @return  none
 */
void PD_Request_Analyse( UINT8 pdo_idx, UINT8 *srccap, UINT16 *current )
{
    UINT32 temp32;

    temp32 = srccap[ (  ( pdo_idx - 1 ) << 2 ) + 0 ] +
            ( (UINT32)srccap[ ( ( pdo_idx - 1 ) << 2 ) + 1 ] << 8 );

    /* Calculation of current values */
    if( current != NULL )
    {
        *current = ( temp32 & 0x00003FF ) * 10;
    }
}

/*********************************************************************
 * @fn      PDO_Request
 *
 * @brief   This function uses to Send the specified PDO.
 *
 * @return  none
 */
void PDO_Request( UINT8 pdo_index )
{
    UINT16 Current,Voltage;
    UINT8  status;
    if ((pdo_index > PDO_Len) || (pdo_index == 0))
    {
        while(1)
        {
            printf("pdo_index error!\r\n");
            Delay_Ms(500);
        }
    }
    else
    {
        memcpy( &PD_Rx_Buf[ 2 ], &Adapter_SrcCap[ 4*(pdo_index-1) + 1 ], 4 );
        PD_PDO_Analyse( 1, &PD_Rx_Buf[ 2 ], &Current, &Voltage );
        g_snk_req_voltage_mV = Voltage;
        g_snk_req_current_mA = Current;
        printf("Request:\r\nCurrent:%d mA\r\nVoltage:%d mV\r\n",Current,Voltage);

        PD_Load_Header( 0x00, DEF_TYPE_REQUEST );
        PD_Rx_Buf[ 5 ] = 0x03;
        PD_Rx_Buf[ 5 ] |= pdo_index<<4;
        PD_Rx_Buf[ 3 ] = PD_Rx_Buf[ 3 ] & 0x03;
        PD_Rx_Buf[ 3 ] |= ( PD_Rx_Buf[ 2 ] << 2 );
        PD_Rx_Buf[ 4 ] = PD_Rx_Buf[ 3 ];
        PD_Rx_Buf[ 4 ] <<= 2;
        PD_Rx_Buf[ 4 ] = PD_Rx_Buf[ 4 ] & 0x0C;
        PD_Rx_Buf[ 4 ] |= ( PD_Rx_Buf[ 2 ] >> 6 );
    }
    status = PD_Send_Handle( &PD_Rx_Buf[ 2 ], 4 );

    if( status == DEF_PD_TX_OK )
    {
        PD_Ctl.PD_State = STA_RX_ACCEPT_WAIT;
    }
    else
    {
        PD_Ctl.PD_State = STA_TX_SOFTRST;
    }
    PD_Ctl.PD_Comm_Timer = 0;
    PD_Ctl.Flag.Bit.PD_Comm_Succ = 1;
}


/*********************************************************************
/****
 * @fn      PD_Main_Proc
 *
 * @brief   This function uses to process PD status.
 *
 * @return  none
 */
void PD_Main_Proc( void )
{
    UINT8  status;
    UINT8  pd_header;
    UINT16 Current;
    UINT8  var;

    /* Receive idle timer count */
    PD_Ctl.PD_BusIdle_Timer += Tmr_Ms_Dlt;

    if(g_role == ROLE_SRC)
    {
        /* ==== SOURCE state machine (WCH style) ==== */
        switch( PD_Ctl.PD_State )
        {
            case STA_DISCONNECT:
				printf("Disconnect\r\n");
				VBUS_disable();
#if(Lowpower==LowpowerON)
#if(Wake_up_mode==USBPDWake_up)
				RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
				EXTI_ClearITPendingBit(EXTI_Line29);
				USBPD->PORT_CC1&=~(CC_PU_Mask);
				USBPD->PORT_CC2&=~(CC_PU_Mask);
				USBPD->PORT_CC1|=CC_PU_80;
				USBPD->PORT_CC2|=CC_PU_80;
				USBPD->PORT_CC1|=CC_CMP_123;
				USBPD->PORT_CC2|=CC_CMP_123;
				USBPD->CONFIG&=~WAKE_POLAR;
				NVIC_DisableIRQ(USBPD_IRQn);
				USBPD->CONFIG|=IE_PD_IO;
				printf("Fell deep sleep\r\n");
				NVIC_EnableIRQ(USBPDWakeUp_IRQn);
				Delay_Ms(100);
				PWR_EnterSTANDBYMode();
#elif(Wake_up_mode==GPIOWake_up)
				RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
				EXTI_ClearITPendingBit(EXTI_Line14);
				EXTI_ClearITPendingBit(EXTI_Line15);
				printf("Fell deep sleep\r\n");
				NVIC_EnableIRQ(EXTI15_8_IRQn);
				Delay_Ms(100);
				PWR_EnterSTANDBYMode();
#endif
#elif(Lowpower==LowpowerOff)

#endif
				PD_PHY_Reset( );
				break;

            case STA_SINK_CONNECT:
                PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
                if( PD_Ctl.PD_Comm_Timer > 159 )
                {
                    PD_Ctl.Flag.Bit.Stop_Det_Chk = 0;
                    PD_Ctl.PD_Comm_Timer = 0;
                    PD_Ctl.Src_Cap_Cnt = 0;
                    PD_Ctl.PD_State = STA_TX_SRC_CAP;
                }
                break;

            case STA_TX_SRC_CAP:
                PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
                if( PD_Ctl.PD_Comm_Timer > 159 )
                {
                    PD_Ctl.Src_Cap_Cnt += 1;
                    if ( PD_Ctl.Src_Cap_Cnt >= 50) PD_Ctl.PD_State = STA_IDLE;

                    PD_Load_Header( 0x00, DEF_TYPE_SRC_CAP );
                    status = PD_Send_Handle(SrcCap_5V5A_Tab, 16 );
                    if( status == DEF_PD_TX_OK )
                    {
                        PD_Ctl.PD_State = STA_RX_REQ_WAIT;
                        printf("Send Source Cap Successfully\r\n");
                    }
                    PD_Ctl.PD_Comm_Timer = 0;
                }
                break;

            case STA_RX_REQ_WAIT:
                PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
                if( PD_Ctl.PD_Comm_Timer > 29 ) PD_Ctl.PD_State = STA_TX_HRST;
                break;

            case STA_TX_ACCEPT:
                PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
                if( PD_Ctl.PD_Comm_Timer > 2 )
                {
                    PD_Load_Header( 0x00, DEF_TYPE_ACCEPT );
                    status = PD_Send_Handle( NULL, 0 );
                    if( status == DEF_PD_TX_OK )
                    {
                        printf("Accept\r\n");
                        float volt = 0;
                        float curr = 0;
                        switch (SinkReqPDOIndex)
                        {
                            case 1: volt = 5.0;  curr = 5.0;  break;
                            case 2: volt = 9.0;  curr = 3.0;  break;
                            case 3: volt = 12.0; curr = 2.25; break;
                            case 4: volt = 15.0; curr = 1.8;  break;
                            default: volt = 5.0; curr = 3.0;  break;
                        }
                        tps55289_set_cdc_compensation(CDC_COMP_0V7);
                        tps55289_set_current_limit(curr);
                        tps55289_set_voltage(volt);
                        tps55289_enable_output(1);

                        PD_Ctl.PD_State = STA_TX_PS_RDY;
                        PD_Ctl.PD_Comm_Timer = 0;
                    }
                    else
                    {
                        PD_Ctl.PD_State = STA_TX_SOFTRST;
                        PD_Ctl.PD_Comm_Timer = 0;
                    }
                }
                break;

            case STA_TX_PS_RDY:
                PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
                if( PD_Ctl.PD_Comm_Timer > 19 )
                {
                    PD_Load_Header( 0x00, DEF_TYPE_PS_RDY );
                    status = PD_Send_Handle( NULL, 0 );
                    if( status == DEF_PD_TX_OK )
                    {
                        printf("PS ready\r\n");
                        PD_Ctl.PD_State = STA_IDLE;
                        PD_Ctl.PD_Comm_Timer = 0;
                    }
                    else
                    {
                        PD_Ctl.PD_State = STA_TX_SOFTRST;
                        PD_Ctl.PD_Comm_Timer = 0;
                    }
                }
                break;

            case STA_TX_SOFTRST:
                /* Send soft reset, if sent successfully, mode unchanged */
                PD_Load_Header( 0x00, DEF_TYPE_SOFT_RESET );
                status = PD_Send_Handle( NULL, 0 );
                PD_Ctl.PD_State = (status == DEF_PD_TX_OK) ? STA_IDLE : STA_TX_HRST;
                PD_Ctl.PD_Comm_Timer = 0;
                break;

            case STA_TX_HRST:
                /* Sending a hard reset */
                PD_Ctl.Flag.Bit.Stop_Det_Chk = 1;
                PD_Phy_SendPack( 0x01, NULL, 0, UPD_HARD_RESET );                   /* send HRST */
                PD_Rx_Mode();                    /* switch to rx mode */
                PD_Ctl.PD_State = STA_IDLE;
                PD_Ctl.PD_Comm_Timer = 0;
                break;

            case STA_TX_VDM_RPI:
                PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
                if( PD_Ctl.PD_Comm_Timer > 10 )
                {
                    PD_Load_Header( 0x00, DEF_TYPE_VENDOR_DEFINED );
                    (void)PD_Send_Handle(VDM_RPI_TX_Tab, 20 );
                    PD_Ctl.PD_Comm_Timer = 0;
                    PD_Ctl.PD_State = STA_IDLE;
                }
                break;

            default:
                break;
        }

        /* Receive message processing */
        if( PD_Ctl.Flag.Bit.Msg_Recvd )
        {
            PD_Ctl.Adapter_Idle_Cnt = 0x00;
            pd_header = PD_Rx_Buf[ 0 ] & 0x1F;

            switch( pd_header )
            {
                case DEF_TYPE_ACCEPT:
                    PD_Ctl.PD_Comm_Timer = 0;
                    if( PD_Ctl.PD_State == STA_RX_ACCEPT_WAIT ) PD_Ctl.PD_State = STA_RX_PS_RDY_WAIT;
                    break;

                case DEF_TYPE_REQUEST:
                    /* Request is received */
                    printf("Handle Request\r\n");
                    Delay_Ms( 2 );
                    PD_Ctl.ReqPDO_Idx =  ( PD_Rx_Buf[ 5 ] & 0x70 ) >> 4;
                    printf("  Request:\r\n  PDO_Idx:%d\r\n",PD_Ctl.ReqPDO_Idx);
                    /* ReqPDO_Idx = 1..4 (PDO #1 ... PDO #4) */
                    if( ( PD_Ctl.ReqPDO_Idx == 0 ) || ( PD_Ctl.ReqPDO_Idx > 4 ) )
                    {
                        PD_Ctl.PD_State = STA_TX_HRST;
                    }
                    else
                    {
                        SinkReqPDOIndex = PD_Ctl.ReqPDO_Idx;
                        PD_Request_Analyse( 1, &PD_Rx_Buf[ 2 ], &Current );
                        printf("  Current:%d mA\r\n",Current);

                        /* PD3.0 / PD2.0 */
                        PD_Ctl.Flag.Bit.PD_Version = (( PD_Rx_Buf[ 0 ] & 0xC0 ) == 0x80) ? 1 : 0;

                        PD_Ctl.PD_State = STA_TX_ACCEPT;
                        PD_Ctl.PD_Comm_Timer = 0;
                    }
                    break;

                case DEF_TYPE_SOFT_RESET:
                    Delay_Ms( 1 );
                    PD_Load_Header( 0x00, DEF_TYPE_ACCEPT );
                    (void)PD_Send_Handle( NULL, 0 );
                    break;

                case DEF_TYPE_VENDOR_DEFINED:
                    if((PD_Rx_Buf[2] == 0x01) && (PD_Rx_Buf[3] == 0xA0) && (PD_Rx_Buf[4] == 0x00) && (PD_Rx_Buf[5] == 0xFF))
                    {
                        PD_Ctl.PD_State = STA_TX_VDM_RPI;
                        PD_Ctl.PD_Comm_Timer = 0;
                    }
                    printf("VDM Command\r\n");
                    break;

                default:
                    printf("Unsupported Command\r\n");
                    break;
            }

            /* Message has been processed, interrupt reception is turned on again */
            PD_Rx_Mode();
            PD_Ctl.Flag.Bit.Msg_Recvd = 0;                    /* Clear the received flag */
            PD_Ctl.PD_BusIdle_Timer = 0;                    /* Idle time cleared */
        }
    }
    else
    {
        /* ==== SINK state machine (WCH style, request 12V if present) ==== */
        switch( PD_Ctl.PD_State )
        {
            case STA_DISCONNECT:
                /* Status: Disconnected */
                printf("Disconnect\r\n");
                PD_PHY_Reset();
                break;

            case STA_SRC_CONNECT:
                /* Status: SRC access */
                /* If SRC_CAP is received within 1S, reset operation is performed */
                PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
                if( PD_Ctl.PD_Comm_Timer > 999 )
                {
                    /* Retry on exception (abort after 5 attempts) */
                    PD_Ctl.Err_Op_Cnt++;
                    if( PD_Ctl.Err_Op_Cnt > 5 )
                    {
                        PD_Ctl.Err_Op_Cnt = 0;
                        PD_Ctl.PD_State = STA_IDLE;
                    }
                    else
                    {
                        PD_PHY_Reset();
                    }
                }
                break;

            case STA_RX_ACCEPT_WAIT:
                /* Status: waiting to receive ACCEPT */
            case STA_RX_PS_RDY_WAIT:
                /* Status: waiting to receive PS_RDY */
                PD_Ctl.PD_Comm_Timer += Tmr_Ms_Dlt;
                if( PD_Ctl.PD_Comm_Timer > 499 )
                {
                    PD_Ctl.Flag.Bit.Stop_Det_Chk = 0;                    /* Enable connection detection*/
                    PD_Ctl.PD_State = STA_TX_SOFTRST;
                    PD_Ctl.PD_Comm_Timer = 0;
                }
                break;

            case STA_TX_SOFTRST:
                /* Status: send software reset */
                PD_Load_Header( 0x00, DEF_TYPE_SOFT_RESET );
                status = PD_Send_Handle( NULL, 0 );
                PD_Ctl.PD_State = (status == DEF_PD_TX_OK) ? STA_IDLE : STA_TX_HRST;
                PD_Ctl.PD_Comm_Timer = 0;
                break;

            case STA_TX_HRST:
                /* Status: Sending a hardware reset */
                PD_Ctl.Flag.Bit.Stop_Det_Chk = 1;
                PD_Phy_SendPack( 0x01, NULL, 0, UPD_HARD_RESET );                   /* send HRST */
                PD_Rx_Mode();                    /* switch to rx mode */
                PD_Ctl.PD_State = STA_IDLE;
                PD_Ctl.PD_Comm_Timer = 0;
                break;

            default:
                break;
        }

        /* Receive message processing */
        if( PD_Ctl.Flag.Bit.Msg_Recvd )
        {
            UINT16 Current_mA, Voltage_mV;

            PD_Ctl.Adapter_Idle_Cnt = 0x00;
            pd_header = PD_Rx_Buf[ 0 ] & 0x1F;

            switch( pd_header )
            {
                case DEF_TYPE_SRC_CAP:
                {
                    Delay_Ms( 5 );
                    PD_Ctl.Flag.Bit.Stop_Det_Chk = 0;                    /* Enable PD disconnection detection */

                    PD_Save_Adapter_SrcCap();

                    /* Analysis of the voltage and current of each PDO group */
                    UINT8 pdo_index = 1;
                    PD_Sink_Stat = 0x01;

                    for (var = 1; var <= PDO_Len; ++var)
                    {
                        PD_PDO_Analyse( var, &PD_Rx_Buf[ 2 ], &Current_mA, &Voltage_mV );
                        /* check if PDO is 12V */
                        if (Voltage_mV == 12000)
                        {
                            pdo_index = var;
                            PD_Sink_Stat = 0x02;
                        }
                        printf("PDO:%d\r\nCurrent:%d mA\r\nVoltage:%d mV\r\n",var,Current_mA,Voltage_mV);
                    }
                    printf("\r\n");

                    /* Different PDO's for different voltages and currents */
                    /* Default application for the first group of PDO, 5V */
                    PDO_Request(pdo_index);
                    break;
                }

                case DEF_TYPE_ACCEPT:
                    /* ACCEPT received */
                    PD_Ctl.PD_State = STA_RX_PS_RDY_WAIT;
                    PD_Ctl.PD_Comm_Timer = 0;
                    break;

                case DEF_TYPE_PS_RDY:
                    /* PS_RDY is received */
                    printf("Success\r\n");
                    g_pd_profile_accepted = 1; /* application flag */
                    g_snk_neg_voltage_100mV = g_snk_req_voltage_mV / 100;
                    g_snk_neg_current_100mA = g_snk_req_current_mA / 100;

					/* lockout: do not re-enter SNK until PB11 is HIGH for PB11_REARM_HIGH_MS */
					g_snk_success_lockout = 1;
					g_pb11_high_stable_ms = 0;

                    /* after success: immediately return to SRC */
                    PDC_SetCcMuxToSink(0);
                    PD_PHY_Reset_Role(ROLE_SRC);
                    g_snk_window_ms = 0;
                    break;

                case DEF_TYPE_GET_SNK_CAP:
                    Delay_Ms( 1 );
                    PD_Load_Header( 0x00, DEF_TYPE_SNK_CAP );
                    (void)PD_Send_Handle( SinkCap_5V1A_Tab, sizeof( SinkCap_5V1A_Tab ) );
                    break;

                case DEF_TYPE_SOFT_RESET:
                    Delay_Ms( 1 );
                    PD_Load_Header( 0x00, DEF_TYPE_ACCEPT );
                    (void)PD_Send_Handle( NULL, 0 );
                    break;

                case DEF_TYPE_GET_SRC_CAP_EX:
                    Delay_Ms( 1 );
                    PD_Load_Header( 0x01, DEF_TYPE_SRC_CAP );
                    (void)PD_Send_Handle( SrcCap_Ext_Tab, sizeof( SrcCap_Ext_Tab ) );
                    break;

                case DEF_TYPE_GET_STATUS:
                    Delay_Ms( 1 );
                    PD_Load_Header( 0x01, DEF_TYPE_GET_STATUS_R );
                    (void)PD_Send_Handle( Status_Ext_Tab, sizeof( Status_Ext_Tab ) );
                    break;

                case DEF_TYPE_VCONN_SWAP:
                    Delay_Ms( 1 );
                    PD_Load_Header( 0x00, DEF_TYPE_REJECT );
                    (void)PD_Send_Handle( NULL, 0 );
                    break;

                default:
                    printf("Unsupported Command\r\n");
                    break;
            }

            /* Message has been processed, interrupt reception is turned on again */
            PD_Rx_Mode();
            PD_Ctl.Flag.Bit.Msg_Recvd = 0;                    /* Clear the received flag */
            PD_Ctl.PD_BusIdle_Timer = 0;                    /* Idle time cleared */
        }
    }
}

/*********************************************************************
/****
 * @fn      PD_Det_Proc
 *
 * @brief   This function uses to process the return value of PD_Detect.
 *
 * @return  none
 */
void PD_Det_Proc( void )
{
    if(g_role == ROLE_SRC)
    {
        /* exact from your source.c */
        UINT8 status = PD_Detect();

        if( PD_Ctl.Flag.Bit.Connected )
        {
            /* PD is connected, detect its disconnection */
            if( status ) PD_Ctl.Det_Cnt = 0;
            else
            {
                PD_Ctl.Det_Cnt++;
                if( PD_Ctl.Det_Cnt >= 5 )
                {
                    PD_Ctl.Det_Cnt = 0;
                    PD_Ctl.Flag.Bit.Connected = 0;
                    if( PD_Ctl.Flag.Bit.Stop_Det_Chk == 0 ) PD_Ctl.PD_State = STA_DISCONNECT;
                }
            }
        }
        else
        {
            /* PD is disconnected, check its connection */
            if( status == 0 ) PD_Ctl.Det_Cnt = 0;
            else              PD_Ctl.Det_Cnt++;

            if( PD_Ctl.Det_Cnt >= 5 )
            {
                PD_Ctl.Det_Cnt = 0;
                PD_Ctl.Flag.Bit.Connected = 1;
                if( PD_Ctl.Flag.Bit.Stop_Det_Chk == 0 )
                {
                    /* Select the corresponding PD channel */
                    if( status == 1 ) USBPD->CONFIG &= ~CC_SEL;
                    else              USBPD->CONFIG |= CC_SEL;

                    if( (USBPD->PORT_CC1 & CC_PD) || (USBPD->PORT_CC2 & CC_PD) )
                    {
                    PD_Ctl.PD_State = STA_SRC_CONNECT;
                    printf("CC%d SRC Connect\r\n",status);
                    }
                    else
                    {
                    PD_Ctl.PD_State = STA_SINK_CONNECT;
                    printf("CC%d SINK Connect\r\n",status);
                    VBUS_set_5V();
                    }

                    PD_Ctl.PD_Comm_Timer = 0;
                }
            }
        }
    }
    else
    {
        /* sink Det_Proc (from your sink.c) */
        UINT8 status;

        if( PD_Ctl.Flag.Bit.Connected )
        {
			/* PD is connected, detect its disconnection */

			/* According to the usage scenario of PD SNK, whether
			 * it is removed or not should be determined by detecting
			 * the Vbus voltage, this code only shows the detection
			 * and the subsequent communication flow. */

			 if (PD_Detect() == 0)
			 {
				 PD_Ctl.Flag.Bit.Connected = 0;
			 }

        }
        else
        {
            /* PD disconnected, check connection */
            status = PD_Detect();
            if( status == 0 )
            {
                PD_Ctl.Det_Cnt = 0;
                PD_Sink_Stat = 0x00;
            }
            else
            {
                PD_Ctl.Det_Cnt++;
            }

            if( PD_Ctl.Det_Cnt >= 5 )
            {
                PD_Ctl.Det_Cnt = 0;
                PD_Ctl.Flag.Bit.Connected = 1;
                if( PD_Ctl.Flag.Bit.Stop_Det_Chk == 0 )
                {
                    if( (USBPD->PORT_CC1 & CC_PD) || (USBPD->PORT_CC2 & CC_PD) )
                    {
						/* Select the corresponding PD channel */
						if( status == 1 ) USBPD->CONFIG &= ~CC_SEL;
						else              USBPD->CONFIG |= CC_SEL;

						PD_Ctl.PD_State = STA_SRC_CONNECT;
						printf("CC%d SRC Connect\r\n",status);
                    }
                    PD_Ctl.PD_Comm_Timer = 0;
                }
            }
        }
    }
}

/*********************************************************************
/****
 * @fn      PD_Phy_SendPack
 *
 * @brief   This function uses to send PD data.
 *
 * @return  none
 */
void PD_Phy_SendPack( UINT8 mode, UINT8 *pbuf, UINT8 len, UINT8 sop )
{
    if ((USBPD->CONFIG & CC_SEL) == CC_SEL ) USBPD->PORT_CC2 |= CC_LVE;
    else                    USBPD->PORT_CC1 |= CC_LVE;

    USBPD->BMC_CLK_CNT = UPD_TMR_TX_48M;
    USBPD->DMA = (UINT32)(UINT8 *)pbuf;
    USBPD->TX_SEL = sop;
    USBPD->BMC_TX_SZ = len;
    USBPD->CONTROL |= PD_TX_EN;
    USBPD->STATUS &= BMC_AUX_INVALID;
    USBPD->CONTROL |= BMC_START;

    /* Determine if you need to wait for the send to complete */
    if( mode )
    {
        /* Wait for the send to complete, this will definitely complete, no need to do a timeout */
        while( (USBPD->STATUS & IF_TX_END) == 0 );
        USBPD->STATUS |= IF_TX_END;

        if((USBPD->CONFIG & CC_SEL) == CC_SEL ) USBPD->PORT_CC2 &= ~CC_LVE;
        else                    USBPD->PORT_CC1 &= ~CC_LVE;

        /* Switch to receive ready to receive GoodCRC */
        USBPD->CONFIG |=  PD_ALL_CLR ;
        USBPD->CONFIG &= ~( PD_ALL_CLR );
        USBPD->CONTROL &= ~ ( PD_TX_EN );
        USBPD->DMA = (UINT32)(UINT8 *)PD_Rx_Buf;
        USBPD->BMC_CLK_CNT = UPD_TMR_RX_48M;
        USBPD->CONTROL |= BMC_START;
    }
}

/*********************************************************************
 * @fn      PD_Load_Header
 *
 * @brief   This function uses to load pd header packets.
 *
 * @return  none
 */
void PD_Load_Header( UINT8 ex, UINT8 msg_type )
{
    /* Message Header
       BIT15 - Extended;
       BIT[14:12] - Number of Data Objects
       BIT[11:9] - Message ID
       BIT8 - PortPower Role/Cable Plug  0: SINK; 1: SOURCE
       BIT[7:6] - Revision, 00: V1.0; 01: V2.0; 10: V3.0;
       BIT5 - Port Data Role, 0: UFP; 1: DFP
       BIT[4:0] - Message Type
    */
    PD_Tx_Buf[ 0 ] = msg_type;
    if( PD_Ctl.Flag.Bit.PD_Role ) PD_Tx_Buf[ 0 ] |= 0x20;

    if( PD_Ctl.Flag.Bit.PD_Version ) PD_Tx_Buf[ 0 ] |= 0x80;   /* PD3.0 */
    else                    PD_Tx_Buf[ 0 ] |= 0x40;   /* PD2.0 */

    PD_Tx_Buf[ 1 ] = PD_Ctl.Msg_ID & 0x0E;
    if( PD_Ctl.Flag.Bit.PR_Role ) PD_Tx_Buf[ 1 ] |= 0x01;
    if( ex )                    PD_Tx_Buf[ 1 ] |= 0x80;
}

/*********************************************************************
/****
 * @fn      PD_Send_Handle
 *
 * @brief   This function uses to handle sending transactions.
 *
 * @return  0:success; 1:fail
 */
UINT8 PD_Send_Handle( UINT8 *pbuf, UINT8 len )
{
    UINT8  pd_tx_trycnt;
    UINT8  cnt;

    if( ( len % 4 ) != 0 ) return DEF_PD_TX_FAIL;      /* Send failed */
    if( len > 28 )         return DEF_PD_TX_FAIL;      /* Send failed */

    cnt = len >> 2;
    PD_Tx_Buf[ 1 ] |= ( cnt << 4 );

    for( cnt = 0; cnt != len; cnt++ )
    {
        PD_Tx_Buf[ 2 + cnt ] = pbuf ? pbuf[ cnt ] : 0;
    }

    pd_tx_trycnt = 4;
    while( --pd_tx_trycnt )                    /* Maximum 3 executions */
    {
        NVIC_DisableIRQ( USBPD_IRQn );
        PD_Phy_SendPack( 0x01, PD_Tx_Buf, ( len + 2 ), UPD_SOP0 );

        /* Set receive timeout 750US */
        cnt = 250;
        while( --cnt )
        {
            if( (USBPD->STATUS & IF_RX_ACT) == IF_RX_ACT)
            {
                USBPD->STATUS |= IF_RX_ACT;
                if( ( USBPD->BMC_BYTE_CNT == 6 ) && ( ( PD_Rx_Buf[ 0 ] & 0x1F ) == DEF_TYPE_GOODCRC ) )
                {
                    PD_Ctl.Msg_ID += 2;
                    break;
                }
            }
            Delay_Us( 3 );
        }
        if( cnt !=0 ) break;
    }

    /* Switch to receive mode */
    PD_Rx_Mode();
    return pd_tx_trycnt ? DEF_PD_TX_OK : DEF_PD_TX_FAIL;
}


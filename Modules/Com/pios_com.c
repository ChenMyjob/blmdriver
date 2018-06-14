/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup PIOS_COM COM layer functions
 * @brief Hardware communication layer
 * @{
 *
 * @file       pios_com.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      COM layer functions
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "uart.h"
#include "pios_com.h"
#include "driver_stm32.h"
#include "string.h"
#include "fifo_buffer.h"

uint32_t comDebugId;
uint32_t comCameraId;
uint32_t comGimInnerId;

uint8_t USART_CONSOLE_TX_BUF[ COM_USART_CONSOLE_TX_BUF_LEN];
uint8_t USART_CONSOLE_RX_BUF[ COM_USART_CONSOLE_RX_BUF_LEN];

enum pios_com_dev_magic {
    PIOS_COM_DEV_MAGIC = 0x436f6d6d,
};

#define COM_DMA_ONCE_MAX	20

struct pios_com_dev {
    enum pios_com_dev_magic magic;
    uint32_t lower_id;
    bool	isDmaUsed;
    const struct com_driver *driver;

#if defined(PIOS_INCLUDE_FREERTOS)
    xSemaphoreHandle tx_sem;
    xSemaphoreHandle txDmaCmpSem;
    xSemaphoreHandle rx_sem;
    xSemaphoreHandle sendbuffer_sem;
#endif

    bool has_rx;
    bool has_tx;

    t_fifo_buffer rx;
    t_fifo_buffer tx;
};

static bool PIOS_COM_validate(struct pios_com_dev *com_dev)
{
    return com_dev && (com_dev->magic == PIOS_COM_DEV_MAGIC);
}

#if defined(PIOS_INCLUDE_FREERTOS)
static struct pios_com_dev *PIOS_COM_alloc(void)
{
    struct pios_com_dev *com_dev;

    com_dev = (struct pios_com_dev *)pvPortMalloc(sizeof(struct pios_com_dev));
    if (!com_dev) {
        return NULL;
    }

    memset(com_dev, 0, sizeof(struct pios_com_dev));
    com_dev->magic = PIOS_COM_DEV_MAGIC;
    return com_dev;
}
#else
static struct pios_com_dev pios_com_devs[PIOS_COM_MAX_DEVS];
static uint8_t pios_com_num_devs;
static struct pios_com_dev *PIOS_COM_alloc(void)
{
    struct pios_com_dev *com_dev;

    if (pios_com_num_devs >= PIOS_COM_MAX_DEVS) {
        return NULL;
    }

    com_dev = &pios_com_devs[pios_com_num_devs++];

    memset(com_dev, 0, sizeof(struct pios_com_dev));
    com_dev->magic = PIOS_COM_DEV_MAGIC;

    return com_dev;
}
#endif /* if defined(PIOS_INCLUDE_FREERTOS) */

static uint16_t PIOS_COM_TxOutCallback(uint32_t context, uint8_t *buf, uint16_t buf_len, uint16_t *headroom, bool *need_yield);
static uint16_t PIOS_COM_RxInCallback(uint32_t context, uint8_t *buf, uint16_t buf_len, uint16_t *headroom, bool *need_yield);
static uint16_t ComTxDmaCallback(uint32_t context,bool *task_woken);
static uint16_t ComRxDmaCallback(uint32_t context,bool *task_woken);
static void PIOS_COM_UnblockRx(struct pios_com_dev *com_dev, bool *need_yield);
static void PIOS_COM_UnblockTx(struct pios_com_dev *com_dev, bool *need_yield);
//extern void IdleIntTrigTaskNotifyGiveISR(uint32_t com_id,BaseType_t * woken);

void GimbalBoardCfgCom(uint32_t usart_port_init,uint8_t *rx_buffer, uint16_t rx_buffer_len, uint8_t *tx_buffer, uint16_t tx_buffer_len,
							const struct com_driver *com_driver,uint32_t *pios_com_id,bool isDmaUsed)
{
	uint32_t pios_usart_id;

	if(USART_Init(&pios_usart_id,(GIMBAL_UART_CFG *)usart_port_init,isDmaUsed))
	{
		DEBUG_Assert(0);
	}
	if (tx_buffer_len != (size_t)-1)
	{ 
		if (PIOS_COM_Init(pios_com_id, com_driver, pios_usart_id,
						  rx_buffer, rx_buffer_len,
						  tx_buffer, tx_buffer_len,isDmaUsed)) {
			DEBUG_Assert(0);
		}
	} else { // rx only port
		if (PIOS_COM_Init(pios_com_id, com_driver, pios_usart_id,
						  rx_buffer, rx_buffer_len,
						  NULL, 0,isDmaUsed)) {
			DEBUG_Assert(0);
		}
	}
}
/**
 * Initialises COM layer
 * \param[out] handle
 * \param[in] driver
 * \param[in] id
 * \return < 0 if initialisation failed
 */
int32_t PIOS_COM_Init(uint32_t *com_id, const struct com_driver *driver, uint32_t lower_id, uint8_t *rx_buffer, uint16_t rx_buffer_len, uint8_t *tx_buffer, uint16_t tx_buffer_len,bool isDmaUsed)
{
    DEBUG_Assert(com_id);
    DEBUG_Assert(driver);

    bool has_rx = (rx_buffer && rx_buffer_len > 0);
    bool has_tx = (tx_buffer && tx_buffer_len > 0);
    DEBUG_Assert(has_rx || has_tx);
    DEBUG_Assert(driver->bind_tx_cb || !has_tx);
    DEBUG_Assert(driver->bind_rx_cb || !has_rx);

    struct pios_com_dev *com_dev;

    com_dev = (struct pios_com_dev *)PIOS_COM_alloc();
    if (!com_dev) {
        goto out_fail;
    }

    com_dev->driver   = driver;
    com_dev->lower_id = lower_id;

    com_dev->has_rx   = has_rx;
    com_dev->has_tx   = has_tx;

    com_dev->isDmaUsed = isDmaUsed;
    if (has_rx) {
        fifoBuf_init(&com_dev->rx, rx_buffer, rx_buffer_len);
#if defined(PIOS_INCLUDE_FREERTOS)
        vSemaphoreCreateBinary(com_dev->rx_sem);
#endif /* PIOS_INCLUDE_FREERTOS */
        if(isDmaUsed)
        {
        	com_dev->driver->bind_DmaRx_cb(lower_id,ComRxDmaCallback,(uint32_t)com_dev);
        	if(com_dev->driver->rxDmaStart)
        	{
        		com_dev->driver->rxDmaStart(com_dev->lower_id,
        									(uint32_t)rx_buffer,fifoBuf_getFree(&com_dev->rx));
        	}
        }else
        {
			(com_dev->driver->bind_rx_cb)(lower_id, PIOS_COM_RxInCallback, (uint32_t)com_dev);
			if (com_dev->driver->rx_start) {
				/* Start the receiver */
				(com_dev->driver->rx_start)(com_dev->lower_id,
											fifoBuf_getFree(&com_dev->rx));
			}
        }
    }

    if (has_tx) {
        fifoBuf_init(&com_dev->tx, tx_buffer, tx_buffer_len);
#if defined(PIOS_INCLUDE_FREERTOS)
        vSemaphoreCreateBinary(com_dev->tx_sem);
        vSemaphoreCreateBinary(com_dev->txDmaCmpSem);
#endif /* PIOS_INCLUDE_FREERTOS */
        if(isDmaUsed)
        {
        	com_dev->driver->bind_DmaTx_cb(lower_id,ComTxDmaCallback,(uint32_t)com_dev);
        }else
        {
        	(com_dev->driver->bind_tx_cb)(lower_id, PIOS_COM_TxOutCallback, (uint32_t)com_dev);
        }
    }
#if defined(PIOS_INCLUDE_FREERTOS)
    com_dev->sendbuffer_sem = xSemaphoreCreateMutex();
#endif /* PIOS_INCLUDE_FREERTOS */

    *com_id = (uint32_t)com_dev;
    return 0;

out_fail:
    return -1;
}

#if defined(PIOS_INCLUDE_FREERTOS)
static void PIOS_COM_UnblockRx(struct pios_com_dev *com_dev, bool *need_yield)
{
    static signed portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;

    xSemaphoreGiveFromISR(com_dev->rx_sem, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken != pdFALSE) {
        *need_yield = true;
    } else {
        *need_yield = false;
    }
}
#else
static void PIOS_COM_UnblockRx(__attribute__((unused)) struct pios_com_dev *com_dev, bool *need_yield)
{
    *need_yield = false;
}
#endif

#if defined(PIOS_INCLUDE_FREERTOS)
static void PIOS_COM_UnblockTx(struct pios_com_dev *com_dev, bool *need_yield)
{
    static signed portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;

    xSemaphoreGiveFromISR(com_dev->tx_sem, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken != pdFALSE) {
        *need_yield = true;
    } else {
        *need_yield = false;
    }
}
#else
static void PIOS_COM_UnblockTx(__attribute__((unused)) struct pios_com_dev *com_dev, bool *need_yield)
{
    *need_yield = false;
}
#endif
#if GIMBAL_TASK_CMDINTERPER == 1

extern void fill_rec_buf(char data);
#endif
static uint16_t PIOS_COM_RxInCallback(uint32_t context, uint8_t *buf, uint16_t buf_len, uint16_t *headroom, bool *need_yield)
{
    struct pios_com_dev *com_dev = (struct pios_com_dev *)context;

    bool valid = PIOS_COM_validate(com_dev);

    DEBUG_Assert(valid);
    DEBUG_Assert(com_dev->has_rx);
    uint16_t bytes_into_fifo;
    if (buf_len == 1) {
        bytes_into_fifo = fifoBuf_putByte(&com_dev->rx, buf[0]);
#if GIMBAL_TASK_CMDINTERPER == 1
        fill_rec_buf(buf[0]);
#endif
    } else {
#if GIMBAL_TASK_CMDINTERPER == 1
    	for(uint8_t i = 0;i<buf_len;i++)
    	{
    		fill_rec_buf(buf[i]);
    	}
#endif
        bytes_into_fifo = fifoBuf_putData(&com_dev->rx, buf, buf_len);
    }
    if (bytes_into_fifo > 0) {
        /* Data has been added to the buffer */
        PIOS_COM_UnblockRx(com_dev, need_yield);
    }

    if (headroom) {
        *headroom = fifoBuf_getFree(&com_dev->rx);
    }

    return bytes_into_fifo;
}

static uint16_t PIOS_COM_TxOutCallback(uint32_t context, uint8_t *buf, uint16_t buf_len, uint16_t *headroom, bool *need_yield)
{
    struct pios_com_dev *com_dev = (struct pios_com_dev *)context;

    bool valid = PIOS_COM_validate(com_dev);

    DEBUG_Assert(valid);
    DEBUG_Assert(buf);
    DEBUG_Assert(buf_len);
    DEBUG_Assert(com_dev->has_tx);

    uint16_t bytes_from_fifo = fifoBuf_getData(&com_dev->tx, buf, buf_len);

    if (bytes_from_fifo > 0) {
        /* More space has been made in the buffer */
        PIOS_COM_UnblockTx(com_dev, need_yield);
    }

    if (headroom) {
        *headroom = fifoBuf_getUsed(&com_dev->tx);
    }

    return bytes_from_fifo;
}

static uint16_t ComTxDmaCallback(uint32_t context,bool *task_woken)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)context;

	bool valid = PIOS_COM_validate(com_dev);

	DEBUG_Assert(valid);
	
	if(!com_dev->isDmaUsed)
	{
		return 0;
	}
//	BaseType_t pxHigherPriorityTaskWoken;
//	xSemaphoreGiveFromISR(com_dev->txDmaCmpSem,&pxHigherPriorityTaskWoken);
//
//	if (pxHigherPriorityTaskWoken != pdFALSE) {
//		*task_woken = true;
//	} else {
//		*task_woken = false;
//	}

	return 0;
}

static uint16_t ComRxDmaCallback(uint32_t context,bool *task_woken)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)context;
//	static signed portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;
//	static signed portBASE_TYPE xHigherPriorityTaskWoken2 = pdFALSE;
	bool valid = PIOS_COM_validate(com_dev);

	DEBUG_Assert(valid);
	
	if(!com_dev->isDmaUsed)
	{
		return 0;
	}
	//do nothing

//	IdleIntTrigTaskNotifyGiveISR(context,&xHigherPriorityTaskWoken1);

//	xSemaphoreGiveFromISR(com_dev->rx_sem, &xHigherPriorityTaskWoken);
//
//	if (xHigherPriorityTaskWoken == pdTRUE)// || xHigherPriorityTaskWoken2 == pdTRUE) {
//	{
//	    *task_woken = true;
//	} else {
//		*task_woken = false;
//	}
	return 0;
}
/**
 * Change the port speed without re-initializing
 * \param[in] port COM port
 * \param[in] baud Requested baud rate
 * \return -1 if port not available
 * \return 0 on success
 */
int32_t PIOS_COM_ChangeBaud(uint32_t com_id, uint32_t baud)
{
    struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

    if (!PIOS_COM_validate(com_dev)) {
        /* Undefined COM port for this board (see pios_board.c) */
        return -1;
    }

    /* Invoke the driver function if it exists */
    if (com_dev->driver->set_baud) {
        com_dev->driver->set_baud(com_dev->lower_id, baud);
    }

    return 0;
}

/**
 * Set control lines associated with the port
 * \param[in] port COM port
 * \param[in] mask Lines to change
 * \param[in] state New state for lines
 * \return -1 if port not available
 * \return 0 on success
 */
int32_t PIOS_COM_SetCtrlLine(uint32_t com_id, uint32_t mask, uint32_t state)
{
    struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

    if (!PIOS_COM_validate(com_dev)) {
        /* Undefined COM port for this board (see pios_board.c) */
        return -1;
    }

    /* Invoke the driver function if it exists */
    if (com_dev->driver->set_ctrl_line) {
        com_dev->driver->set_ctrl_line(com_dev->lower_id, mask, state);
    }

    return 0;
}

/**
 * Set control lines associated with the port
 * \param[in] port COM port
 * \param[in] ctrl_line_cb Callback function
 * \param[in] context context to pass to the callback function
 * \return -1 if port not available
 * \return 0 on success
 */
int32_t PIOS_COM_RegisterCtrlLineCallback(uint32_t com_id, pios_com_callback_ctrl_line ctrl_line_cb, uint32_t context)
{
    struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

    if (!PIOS_COM_validate(com_dev)) {
        /* Undefined COM port for this board (see pios_board.c) */
        return -1;
    }

    /* Invoke the driver function if it exists */
    if (com_dev->driver->bind_ctrl_line_cb) {
        com_dev->driver->bind_ctrl_line_cb(com_dev->lower_id, ctrl_line_cb, context);
    }

    return 0;
}


static int32_t PIOS_COM_SendBufferNonBlockingInternal(struct pios_com_dev *com_dev, const uint8_t *buffer, uint16_t len)
{
	uint16_t bytes_into_fifo = 0;
    DEBUG_Assert(com_dev);
    DEBUG_Assert(com_dev->has_tx);
    if (com_dev->driver->available && !com_dev->driver->available(com_dev->lower_id)) {
        /*
         * Underlying device is down/unconnected.
         * Dump our fifo contents and act like an infinite data sink.
         * Failure to do this results in stale data in the fifo as well as
         * possibly having the caller block trying to send to a device that's
         * no longer accepting data.
         */
        fifoBuf_clearData(&com_dev->tx);
        return len;
    }

	/* More data has been put in the tx buffer, make sure the tx is started */
	if(com_dev->isDmaUsed)
	{
//		if (xSemaphoreTake(com_dev->txDmaCmpSem,MS2TICKS(1)) == pdTRUE)
//		{
//			fifoBuf_flush(&com_dev->tx);
//			if(len < fifoBuf_getFree(&com_dev->tx))
//			{
//				bytes_into_fifo = fifoBuf_putData(&com_dev->tx, buffer, len);
//				if(com_dev->driver->txDmaStart)
//				{
//					com_dev->driver->txDmaStart(com_dev->lower_id,(uint32_t)com_dev->tx.buf_ptr,bytes_into_fifo);
//				}
//			}
//		}else
//		{
//		    if(com_dev->driver->txDMACount(com_dev->lower_id)==0)
//		    {
//		        return -1;
//		    }else
//		    {
//		        return -2;
//		    }
//		}
	}else if (com_dev->driver->tx_start) {
		if(len > fifoBuf_getFree(&com_dev->tx))
		{
			return -2;
		}
		
		bytes_into_fifo = fifoBuf_putData(&com_dev->tx, buffer, len);
		if(bytes_into_fifo >0)
		{
			com_dev->driver->tx_start(com_dev->lower_id,fifoBuf_getUsed(&com_dev->tx));
		}
	}
    return bytes_into_fifo;
}

/**
 * Sends a package over given port
 * \param[in] port COM port
 * \param[in] buffer character buffer
 * \param[in] len buffer length
 * \return -1 if port not available
 * \return -2 if non-blocking mode activated: buffer is full
 *            caller should retry until buffer is free again
 * \return -3 another thread is already sending, caller should
 *            retry until com is available again
 * \return number of bytes transmitted on success
 */
int32_t PIOS_COM_SendBufferNonBlocking(uint32_t com_id, const uint8_t *buffer, uint16_t len)
{
    struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;
    int32_t ret = -1;
    if (!PIOS_COM_validate(com_dev)) {
        /* Undefined COM port for this board (see pios_board.c) */
        return -1;
    }
#if defined(PIOS_INCLUDE_FREERTOS)
    if (xSemaphoreTake(com_dev->sendbuffer_sem, MS2TICKS(1)) != pdTRUE) {
        return -3;
    }
#endif /* PIOS_INCLUDE_FREERTOS */
    if(len>0)
    {
    	ret = PIOS_COM_SendBufferNonBlockingInternal(com_dev, buffer, len);
    }
#if defined(PIOS_INCLUDE_FREERTOS)
    xSemaphoreGive(com_dev->sendbuffer_sem);
#endif /* PIOS_INCLUDE_FREERTOS */
    return ret;
}


/**
 * Sends a package over given port
 * (blocking function)
 * \param[in] port COM port
 * \param[in] buffer character buffer
 * \param[in] len buffer length
 * \return -1 if port not available
 * \return -2 if mutex can't be taken;
 * \return -3 if data cannot be sent in the max allotted time of 5000msec
 * \return number of bytes transmitted on success
 */
int32_t PIOS_COM_SendBuffer(uint32_t com_id, const uint8_t *buffer, uint16_t len)
{
    struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

    if (!PIOS_COM_validate(com_dev)) {
        /* Undefined COM port for this board (see pios_board.c) */
        return -1;
    }
    DEBUG_Assert(com_dev->has_tx);
#if defined(PIOS_INCLUDE_FREERTOS)
    if (xSemaphoreTake(com_dev->sendbuffer_sem, 5) != pdTRUE) {
        return -2;
    }
#endif /* PIOS_INCLUDE_FREERTOS */
    uint32_t max_frag_len  = fifoBuf_getSize(&com_dev->tx);
    uint32_t bytes_to_send = len;
    while (bytes_to_send) {
        uint32_t frag_size;

        if (bytes_to_send > max_frag_len) {
            frag_size = max_frag_len;
        } else {
            frag_size = bytes_to_send;
        }
        int32_t rc = PIOS_COM_SendBufferNonBlockingInternal(com_dev, buffer, frag_size);
        if (rc >= 0) {
            bytes_to_send -= rc;
            buffer += rc;
        } else {
            switch (rc) {
            case -1:
#if defined(PIOS_INCLUDE_FREERTOS)
                xSemaphoreGive(com_dev->sendbuffer_sem);
#endif /* PIOS_INCLUDE_FREERTOS */
                /* Device is invalid, this will never work */
                return -1;

            case -2:
                /* Device is busy, wait for the underlying device to free some space and retry */
                /* Make sure the transmitter is running while we wait */
                if (com_dev->driver->tx_start) {
                    (com_dev->driver->tx_start)(com_dev->lower_id,
                                                fifoBuf_getUsed(&com_dev->tx));
                }
#if defined(PIOS_INCLUDE_FREERTOS)
                if (xSemaphoreTake(com_dev->tx_sem, 5000) != pdTRUE) {
                    xSemaphoreGive(com_dev->sendbuffer_sem);
                    return -3;
                }
#endif
                continue;
            default:
                /* Unhandled return code */
#if defined(PIOS_INCLUDE_FREERTOS)
                xSemaphoreGive(com_dev->sendbuffer_sem);
#endif /* PIOS_INCLUDE_FREERTOS */
                return rc;
            }
        }
    }
#if defined(PIOS_INCLUDE_FREERTOS)
    xSemaphoreGive(com_dev->sendbuffer_sem);
#endif /* PIOS_INCLUDE_FREERTOS */
    return len;
}

/**
 * Sends a single character over given port
 * \param[in] port COM port
 * \param[in] c character
 * \return -1 if port not available
 * \return -2 buffer is full
 *            caller should retry until buffer is free again
 * \return 0 on success
 */
int32_t PIOS_COM_SendCharNonBlocking(uint32_t com_id, char c)
{
    return PIOS_COM_SendBufferNonBlocking(com_id, (uint8_t *)&c, 1);
}

/**
 * Sends a single character over given port
 * (blocking function)
 * \param[in] port COM port
 * \param[in] c character
 * \return -1 if port not available
 * \return 0 on success
 */
int32_t PIOS_COM_SendChar(uint32_t com_id, char c)
{
    return PIOS_COM_SendBuffer(com_id, (uint8_t *)&c, 1);
}

/**
 * Sends a string over given port
 * \param[in] port COM port
 * \param[in] str zero-terminated string
 * \return -1 if port not available
 * \return -2 buffer is full
 *         caller should retry until buffer is free again
 * \return 0 on success
 */
int32_t PIOS_COM_SendStringNonBlocking(uint32_t com_id, const char *str)
{
    return PIOS_COM_SendBufferNonBlocking(com_id, (uint8_t *)str, (uint16_t)strlen(str));
}

/**
 * Sends a string over given port
 * (blocking function)
 * \param[in] port COM port
 * \param[in] str zero-terminated string
 * \return -1 if port not available
 * \return 0 on success
 */
int32_t PIOS_COM_SendString(uint32_t com_id, const char *str)
{
    return PIOS_COM_SendBuffer(com_id, (uint8_t *)str, strlen(str));
}

/**
 * Sends a formatted string (-> printf) over given port
 * \param[in] port COM port
 * \param[in] *format zero-terminated format string - 128 characters supported maximum!
 * \param[in] ... optional arguments,
 *        128 characters supported maximum!
 * \return -2 if non-blocking mode activated: buffer is full
 *         caller should retry until buffer is free again
 * \return 0 on success

int32_t PIOS_COM_SendFormattedStringNonBlocking(uint32_t com_id, const char *format, ...)
{
    uint8_t buffer[128]; // TODO: tmp!!! Provide a streamed COM method later!

    va_list args;

    va_start(args, format);
    vsprintf((char *)buffer, format, args);
    return PIOS_COM_SendBufferNonBlocking(com_id, buffer, (uint16_t)strlen((char *)buffer));
}
 */
/**
 * Sends a formatted string (-> printf) over given port
 * (blocking function)
 * \param[in] port COM port
 * \param[in] *format zero-terminated format string - 128 characters supported maximum!
 * \param[in] ... optional arguments,
 * \return -1 if port not available
 * \return 0 on success

int32_t PIOS_COM_SendFormattedString(uint32_t com_id, const char *format, ...)
{
    uint8_t buffer[128]; // TODO: tmp!!! Provide a streamed COM method later!
    va_list args;

    va_start(args, format);
    vsprintf((char *)buffer, format, args);
    return PIOS_COM_SendBuffer(com_id, buffer, (uint16_t)strlen((char *)buffer));
}
 */
/**
 * Transfer bytes from port buffers into another buffer
 * \param[in] port COM port
 * \returns Byte from buffer
 */
uint16_t PIOS_COM_ReceiveBuffer(uint32_t com_id, uint8_t *buf, uint16_t buf_len, uint32_t timeout_ms)
{
    DEBUG_Assert(buf);
    DEBUG_Assert(buf_len);
    uint16_t bytes_from_fifo;

    struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

    if (!PIOS_COM_validate(com_dev)) {
        /* Undefined COM port for this board (see pios_board.c) */
        DEBUG_Assert(0);
    }
    DEBUG_Assert(com_dev->has_rx);

check_again:

	
	if(com_dev->isDmaUsed)
	{
		com_dev->rx.wr = com_dev->rx.buf_size - com_dev->driver->rxDMACount(com_dev->lower_id);
		if(com_dev->rx.wr >= com_dev->rx.buf_size)
		{
			com_dev->rx.wr = 0;
		}
	}
    bytes_from_fifo = fifoBuf_getData(&com_dev->rx, buf, buf_len);

    if (bytes_from_fifo == 0) {
        /* No more bytes in receive buffer */
        /* Make sure the receiver is running while we wait */
    	if(!com_dev->isDmaUsed)
    	{
			if (com_dev->driver->rx_start) {
				/* Notify the lower layer that there is now room in the rx buffer */
				(com_dev->driver->rx_start)(com_dev->lower_id,
											fifoBuf_getFree(&com_dev->rx));
			}
    	
			if (timeout_ms > 0) {
#if defined(PIOS_INCLUDE_FREERTOS)
				if (xSemaphoreTake(com_dev->rx_sem, timeout_ms / portTICK_RATE_MS) == pdTRUE) {
					/* Make sure we don't come back here again */
					timeout_ms = 0;
					goto check_again;
				}
#else
				HAL_Delay(1);
				timeout_ms--;
				goto check_again;
#endif
			}
        }else
        {
        	if (timeout_ms > 0) {
        	#if defined(PIOS_INCLUDE_FREERTOS)
				if (xSemaphoreTake(com_dev->rx_sem, timeout_ms / portTICK_RATE_MS) == pdTRUE)
				{
					/* Make sure we don't come back here again */
					timeout_ms = 0;
					goto check_again;
				}
        	#else
				HAL_Delay(1);
				timeout_ms--;
				goto check_again;
        	#endif
        	}
        }
    }

    /* Return received byte */
    return bytes_from_fifo;
}
/**
 * Transfer bytes from fifo_buffer which already received
 * \param[in] port COM port
 * \returns Byte from buffer
 */
uint16_t PIOS_COM_ReceiveByteLen(uint32_t com_id)
{
	uint16_t bytes_from_fifo;
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;
	
	if (!PIOS_COM_validate(com_dev)) {
	   /* Undefined COM port for this board (see pios_board.c) */
	   DEBUG_Assert(0);
	}
	if(com_dev->isDmaUsed)
	{
		com_dev->rx.wr = com_dev->rx.buf_size - com_dev->driver->rxDMACount(com_dev->lower_id);
	}
	bytes_from_fifo = fifoBuf_getUsed(&com_dev->rx);

    return bytes_from_fifo;
}
/**
 * / get a data byte from the buffer without removing it
 * \param[in] port COM port
 * \returns Byte from buffer
 */
bool PIOS_COM_ReceiveBytePeek(uint32_t com_id,uint8_t *byte)
{
	uint16_t bytes_from_fifo;
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;
	
	if (!PIOS_COM_validate(com_dev)) {
	   /* Undefined COM port for this board (see pios_board.c) */
	   DEBUG_Assert(0);
	}
	if(com_dev->isDmaUsed)
	{
		com_dev->rx.wr = com_dev->rx.buf_size - com_dev->driver->rxDMACount(com_dev->lower_id);
	}
    bytes_from_fifo = fifoBuf_getUsed(&com_dev->rx);
    
    if(bytes_from_fifo == 0)
    {
    	return false;
    }
    
    *byte = fifoBuf_getBytePeek(&com_dev->rx);
    
    return true;
}

void PIOS_COM_Flush_Rx(uint32_t com_id)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

	if (!PIOS_COM_validate(com_dev)) {
	   /* Undefined COM port for this board (see pios_board.c) */
	   DEBUG_Assert(0);
	}
	if(com_dev->isDmaUsed)
	{
		com_dev->rx.wr = com_dev->rx.buf_size - com_dev->driver->rxDMACount(com_dev->lower_id);
		if(com_dev->rx.wr == com_dev->rx.buf_size)
		{
			com_dev->rx.wr = 0;
		}
		com_dev->rx.rd =  com_dev->rx.wr;
	}else
	{
		fifoBuf_flush(&com_dev->rx);
	}
}

void PIOS_COM_Flush_Tx(uint32_t com_id)
{
	struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

	if (!PIOS_COM_validate(com_dev)) {
	   /* Undefined COM port for this board (see pios_board.c) */
	   DEBUG_Assert(0);
	}

	fifoBuf_flush(&com_dev->tx);
}
/**
 * Query if a com port is available for use.  That can be
 * used to check a link is established even if the device
 * is valid.
 */
bool PIOS_COM_Available(uint32_t com_id)
{
    struct pios_com_dev *com_dev = (struct pios_com_dev *)com_id;

    if (!PIOS_COM_validate(com_dev)) {
        return false;
    }

    // If a driver does not provide a query method assume always
    // available if valid
    if (com_dev->driver->available == NULL) {
        return true;
    }

    return (com_dev->driver->available)(com_dev->lower_id);
}

#include "protocol.h"
int _write(int fd, char *ptr, int len)
{
	fifoBuf_putData(&gbConsoleBuffer,(uint8_t *)ptr, len);
//	PIOS_COM_SendBufferNonBlocking(comDebugId, (uint8_t *)ptr, len);
	return len;
}

/**
 * @}
 * @}
 */

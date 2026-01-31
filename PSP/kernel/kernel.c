//
// PSP Serial Example
// Hobbled together in 2025 - Stephen Cooper
// 
// This is a combination of code, in no specific order, by:
// PSP SERIAL
//    The OP-Ditto team https://github.com/Operation-DITTO
//    and TokyoDrift originally posted on the ACIDMOD forum later uploaded to GitHub
//       https://github.com/unraze/PSXControllerToPSP
//       https://acidmods.com/forum/index.php/topic,34966.0.html?PHPSESSID=83a0ebb0570430cac654db79d72e66dc
// PSP KUBRIDGE EXAMPLE
//    By Joel16 from the PSP Homebrew Discord and PR'd into the PSP SDK
//    https://github.com/pspdev/pspsdk/pull/258
//
// The init logic is by TokyoDrift the rest is OP-Ditto
//

#include <pspsdk.h>
#include <pspintrman_kernel.h>
#include <pspintrman.h>
#include <pspsyscon.h>

// no changes here...the same good old consts
#define PSP_UART4_DIV1 0xBE500024
#define PSP_UART4_DIV2 0xBE500028
#define PSP_UART4_CTRL 0xBE50002C
#define PSP_UART_CLK   96000000
#define SIO_CHAR_RECV_EVENT  0x01

#define PSP_UART4_FIFO 0xBE500000
#define PSP_UART4_STAT 0xBE500018

#define PSP_UART_TXFULL  0x20
#define PSP_UART_RXEMPTY 0x10


static SceUID sio_eventflag = -1;

// CIRCULAR FIFO FUNCTIONS-------------------------
unsigned short int w = 0;
unsigned short int r = 0;
unsigned short int buf[255]; // 0..255 = 256 bytes


void fWrite(unsigned short int b){
   buf[w] = b;
   w++;
   if (w==r) r++; // CHECK THIS... "r++" should not be interrupted...consider about stopping ALL interrupts in intr handler
}

int fRead(){
   if (w==r) return -1;
   int b = buf[r];
   r++; // CHECK THIS... "r++" should not be interrupted...consider about stopping ALL interrupts in intr handler
   return b;
}

// END CIRCULAR FIFO FUNCTIONS-------------------------
//
// Module info - remove these if embedding into application!
//
PSP_MODULE_INFO("kernel", PSP_MODULE_KERNEL, 1, 1);
PSP_NO_CREATE_MAIN_THREAD(); 

//
// Stubs for sio.c
// 
void 	sceHprmEnd(void);
void 	sceHprmReset(void);
void 	sceHprmInit(void);
void 	sceSysregUartIoEnable(int);

void _sioInit(void)
{
   /* Shut down the remote driver */
   sceHprmEnd();
   /* Enable UART 4 */
   sceSysregUartIoEnable(4);
   /* Enable remote control power */
   sceSysconCtrlHRPower(1);
}

int intr_handler(void *arg)
{
   // disable interrupt...we don't want SIO to call intr_handler again while it's already running
   // don't know if it's really needed here, but i remember this was a must in pc programming
   // MAYBE i'm better use "int intrs = pspSdkDisableInterrupts();" (disable ALL intrs) to handle reader/writer conflicts

   sceKernelDisableIntr(PSP_HPREMOTE_INT);

   /* Read out the interrupt state and clear it */
   u32 stat = _lw(0xBE500040);
   _sw(stat, 0xBE500044);

   if(!(_lw(PSP_UART4_STAT) & PSP_UART_RXEMPTY)) {
      fWrite(_lw(PSP_UART4_FIFO));
      sceKernelSetEventFlag(sio_eventflag, SIO_CHAR_RECV_EVENT); // set "we got something!!" flag
   }

   sceKernelEnableIntr(PSP_HPREMOTE_INT); // re-enable interrupt
   // MAYBE i'm better use "pspSdkEnableInterrupts(intrs);"
   return -1;
}

void setBaud(int baud){ // no need to export this....always call sioInit()...

   int div1, div2; // low, high bits of divisor value

   div1 = PSP_UART_CLK / baud;
   div2 = div1 & 0x3F;
   div1 >>= 6;

   _sw(div1, PSP_UART4_DIV1);
   _sw(div2, PSP_UART4_DIV2);
   _sw(0x60, PSP_UART4_CTRL); // ?? someone do it with 0x70
}

void pspUARTInit(int baud)
{
   unsigned int k1 = pspSdkSetK1(0);

   _sioInit();

   sio_eventflag = sceKernelCreateEventFlag("SioShellEvent", 0, 0, 0);
               
   sceKernelRegisterIntrHandler(PSP_HPREMOTE_INT, 1, intr_handler, NULL, NULL);
   sceKernelEnableIntr(PSP_HPREMOTE_INT);
   sceKernelDelayThread(2000000);
   setBaud(baud);

   pspSdkSetK1(k1);
}

void pspUARTTerminate()
{
	unsigned int k1 = pspSdkSetK1(0);

	sceSysconCtrlHRPower(0);
	sceKernelDeleteEventFlag(sio_eventflag);
	sceKernelReleaseIntrHandler(PSP_HPREMOTE_INT);
	sceKernelDisableIntr(PSP_HPREMOTE_INT);

	pspSdkSetK1(k1);
}

int pspUARTRead(void)
{
   unsigned int k1 = pspSdkSetK1(0);

   int ch;
   u32 result;
   SceUInt timeout;

   timeout = 100000;

   ch = fRead();

   if(ch == -1)
   {
      sceKernelWaitEventFlag(sio_eventflag, SIO_CHAR_RECV_EVENT, PSP_EVENT_WAITOR|PSP_EVENT_WAITCLEAR, &result, &timeout); //timeout could be null=forever
      ch = fRead();
   }

   pspSdkSetK1(k1);
   return ch;
}

void pspUARTWrite(int ch)
{
   // as you see this is _blocking_...not an issue for
   // normal use as everithing doing I/O
   // should run in its own thread..in addition, HW FIFO isn't
   // working at all by now, so queue should not be that long :)
	while(_lw(PSP_UART4_STAT) & PSP_UART_TXFULL){
		sceKernelDelayThread(100);
	}
   _sw(ch, PSP_UART4_FIFO);
}

void pspUARTWriteString(const char *data, int len){
   unsigned int k1 = pspSdkSetK1(0);

   int i;
   for(i = 0; i < len; i++)
   {
      pspUARTWrite(data[i]);
   }

   pspSdkSetK1(k1);
}

void pspUARTResetBuff() {
   // disable SIO interrupt...prevents resetFBuf() from being interrupted
   // by SIO events that would result in a data inconsistency.
   sceKernelDisableIntr(PSP_HPREMOTE_INT);
   w = 0;
   r = 0;
   sceKernelEnableIntr(PSP_HPREMOTE_INT); // re-enable interrupt
}

int module_start(SceSize args, void *argp) {
    return 0;
}

int module_stop(void) {
    return 0;
}

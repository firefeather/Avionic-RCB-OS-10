/**
  ******************************************************************************
  * @file    Project/STM32F10x_StdPeriph_Template/main.c 
  * @author  MCD Application Team
  * @version V3.5.0
  * @date    08-April-2011
  * @brief   Main program body
  ******************************************************************************
  * @attention
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
  *
  * <h2><center>&copy; COPYRIGHT 2011 STMicroelectronics</center></h2>
  ******************************************************************************
  */  

/* Includes ------------------------------------------------------------------*/
#include "stm32f10x.h"
#include "time.h"
#include "spi.h"
#include "lcd.h"
#include "Ext_Flash.h"
#include "ff.h"
#include "mass_mal.h"
#include "usb_lib.h"
#include "hw_config.h"
#include "usb_pwr.h"
#include "usb_istr.h"
#include "page.h"
#include "adc.h"
#include "beep.h"
#include "key.h"
#include "rf_spi.h"
#include "cc2500.h"
#include "cfg.h"
#include "ppm_decode.h"
#include "Tx.h"

CSTR StickNotCal[]={"摇杆未校准!","calibrate sticks"};
CSTR CloseIdleMode[]={"请关闭特技模式!","idle-up warning"};
CSTR ThrNotReset[]={"油门摇杆未复位!","throttle warning"};

//系统中断管理
void NVIC_Configuration(void)
{ 
	/* 2 bit for pre-emption priority, 2 bits for subpriority */
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
	/*
		中断源：
				1：系统定时器中断 					0----0�
				2：PPM输入外部中断、Rx接收外部中断 	0----1
				3：Tx发射定时中断	  				1----0
				4: Tx处理定时中断					2----0
	*/
}

void GPIO_Comfiguration(void)
{	
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO,ENABLE);
	GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);//将PA15、PB3、PB4重映射为普通IO
}

u8 ParamLoaded;

void PowerManager(void)
{
	static u16 last_tx_v = 0;
	static u16 power_div = 0;
	
	if(power_div++<10)	return;//5*10=50ms
	power_div = 0;
	
	TxBatVol = ADC_AvgValue[AD_BAT]*0.4937;//单位0.01V
	
	if(TxBatVol==0)	return;
	if(ParamLoaded==0)	return;
	
	if(last_tx_v/TxBatVol>=2)
	{
		DelayMs(80);
		__disable_irq();//关闭总中断
		Beep_Set_Vibration(0);//关闭震动电机
		
		ModelSave(TxSys.ModelNo);
		SaveCfg(); 
		while(1);
	}
	else
	{
		last_tx_v = TxBatVol;
	}
}

//系统处理
void SysProc(void)
{
	KeyScanHandler(); 
	BeepHandler(); 
	BatteryHandler();			
	TimerProc();
	TrimProc();
	PpmInCtl(Model.PpmIn);
	RxK8taUnpacking();
	LCDHandler();
}

/**
  * @brief  Main program.
  * @param  None
  * @retval None
  */
int main(void)
{
	FATFS fs;//逻辑磁盘工作区
	FRESULT res;
	u16 i;
	char *msg = 0;
	
	NVIC_Configuration();
	GPIO_Comfiguration();
	ADC1_DMA_Configuration();
	Time_Init();//必须在LCD之前初始化
	DelayMs(500);
	Key_Configuration();
	Beep_Cofiguration();
	SPI2_Configuration();//初始化SST25、LCD的接口
	SST25_Init();
	LCD_Init();
	RF_SPI_Configuration();
	TxTimerCfg();
	TxProcTimerInit();
	PpmInCfg();
	
	res = f_mount(0,&fs);//挂载SPI Flash
	Mass_Memory_Size[0]=2*1024*1024;//FLASH 大小为2M字节
    Mass_Block_Size[0] =512;//因为我们在Init里面设置了SD卡的操作字节为512个,所以这里一定是512个字节.
    Mass_Block_Count[0]=Mass_Memory_Size[0]/Mass_Block_Size[0];
	
	if((res!=FR_OK)||(Key_ScanOnce()==KEY_EXT))//文件系统出错或按住EXT开机
	{
		LCD_Clr_All();
		LcdDrawUsbLogo(0,4);
		LCD_Refresh_Frame();
		LCD_Refresh_Frame();
		LCD_Refresh_Frame();
		LCD_Refresh_Frame();
		LCD_SetBL(16);
		USB_Interrupts_Config();    
		Set_USBClock();   
		USB_Init();	
		
		while(1);
	}
	 
	LoadCfg();//加载遥控器参数
	ModelLoad(TxSys.ModelNo,0);//加载模型数据,不初始化通信功能
	ParamLoaded = 1;//参数已加载完毕
	BeepMusic(MusicStartup);
	LCD_Disp_BMP(0,0,"res/logo.bmp");
	for(i=0;i<2000;i++)
	{
		SysProc();
	}
	//定时器复位
	TxTimer.Reset=1;

	//摇杆校准检测
	if(TxSys.StkCalied==0)	PageAlertModel(StickNotCal[TxSys.Language],0);
	
	if(SwTstZYSState()==ZYS_LEFT)
	{
		if(!(TxSys.StkType%2))//0/2
		{
			TxSys.StkType = 1;//模式2
		}
	}
	else
	{
		if(TxSys.StkType%2)//1/3
		{
			TxSys.StkType = 0;//模式1
		}
	}
	   	
	//检测油门和开关
	do{
		msg = 0;
		if((FlyMode==FM_IDL1)||(FlyMode==FM_IDL2))	msg = (char *)CloseIdleMode[TxSys.Language];
		else if(TxSys.StkCalied==1 && StickValue[2]>THRSAFE)	msg = (char *)ThrNotReset[TxSys.Language];
		PageAlertModel(msg,100);
	}while(msg);
	
	//重新加载模型数据,为了始化通信功能
	ModelLoad(TxSys.ModelNo,1);
	
	PageSet(PageMain,PV_INIT);

	while(1)
	{
		SysProc();
		PageStack[PageStackIdx](PV_RUN);
	}
}

#ifdef  USE_FULL_ASSERT

/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t* file, uint32_t line)
{ 
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */

  /* Infinite loop */
}
#endif

/******************* (C) COPYRIGHT 2011 STMicroelectronics *****END OF FILE****/

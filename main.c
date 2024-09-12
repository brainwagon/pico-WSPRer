/////////////////////////////////////////////////////////////////////////////
//
//  PROJECT PAGE
//  https://github.com/EngineerGuy314/pico-WSPRer
//
//  Much of the code forked from work by
//  Roman Piksaykin [piksaykin@gmail.com], R2BDY
//  https://github.com/RPiks/pico-WSPR-tx
///////////////////////////////////////////////////////////////////////////////
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/multicore.h"
#include "hf-oscillator/lib/assert.h"
#include "hardware/flash.h"
#include <WSPRbeacon.h>
#include <defines.h>
#include <piodco.h>
#include "debug/logutils.h"
#include <protos.h>
#include <math.h>
#include <utilities.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/uart.h"
#include "hardware/i2c.h"
#include "pico/sleep.h"      
#include "hardware/rtc.h" 
#include "onewire/onewire_library.h"    // onewire library functions
#include "onewire/ow_rom.h"             // onewire ROM command codes
#include "onewire/ds18b20.h"            // ds18b20 function codes


WSPRbeaconContext *pWSPR;

char _callsign[7];        //these get set via terminal, and then from NVRAM on boot
char _id13[3];
char _start_minute[2];
char _lane[2];
char _suffix[2];
char _verbosity[2];
char _oscillator[2];
char _custom_PCB[2];   
char _TELEN_config[5];     
char _battery_mode[2];

static uint32_t telen_values[4];  //consolodate in an array to make coding easier
static absolute_time_t LED_sequence_start_time;
static int GPS_PPS_PIN;     //these get set based on values in defines.h, and also if custom PCB selected in user menu
static int RFOUT_PIN;
static int GPS_ENABLE_PIN;
uint gpio_for_onewire;
int force_transmit = 0;
uint32_t fader; //for creating "breathing" effect on LED to indicate corruption of NVRAM
uint32_t fade_counter;
int maxdevs = 10;
uint64_t OW_romcodes[10];
float onewire_values[10];
int number_of_onewire_devs;
OW one_wire_interface;   //onewire interface

PioDco DCO = {0};

int main()
{
	InitPicoClock();			    // Sets the system clock generator
	StampPrintf("\n");DoLogPrint(); // needed asap to wake up the USB stdio port (because StampPrintf includes stdio_init_all();). why though?
	
	gpio_init(LED_PIN); 
	gpio_set_dir(LED_PIN, GPIO_OUT); //initialize LED output
		
	for (int i=0;i < 20;i++)     //do some blinky on startup, allows time for power supply to stabilize before GPS unit enabled
	{
        gpio_put(LED_PIN, 1); 
        sleep_ms(100);
        gpio_put(LED_PIN, 0);
		sleep_ms(100);
	}
	read_NVRAM();				//reads values of _callsign,  _verbosity etc from NVRAM. MUST READ THESE *BEFORE* InitPicoPins
if (check_data_validity()==-1)  //if data was bad, breathe LED for 10 seconds and reboot. or if user presses a key enter setup
	{
	printf("\nBAD values in NVRAM detected! will reboot in 10 seconds... press any key to enter user-setup menu..\n");
	fader=0;fade_counter=0;
			while (getchar_timeout_us(0)==PICO_ERROR_TIMEOUT) //looks for input on USB serial port only @#$%^&!! they changed this function in SDK 2.0!. used to use -1 for no input, now its -2 PICO_ERROR_TIMEOUT
			{
			 fader+=1;
			 if ((fader%5000)>(fader/100))
			 gpio_put(LED_PIN, 1); 
				else
			 gpio_put(LED_PIN, 0);	
			 if (fader>500000) 
				{
					fader=0;
					fade_counter+=1;
						if (fade_counter>10) {watchdog_enable(100, 1);for(;;)	{} } //after ~10 secs force a reboot
				}
			}	
		DCO._pGPStime->user_setup_menu_active=1;	//if we get here, they pressed a button
		user_interface();  
	}
	
	InitPicoPins();				// Sets GPIO pins roles and directions and also ADC for voltage and temperature measurements (NVRAM must be read BEFORE this, otherwise dont know how to map IO)
	I2C_init();
    printf("\npico-WSPRer version: %s %s\nWSPR beacon init...",__DATE__ ,__TIME__);	//messages are sent to USB serial port, 115200 baud

	uint32_t XMIT_FREQUENCY;
	switch(_lane[0])                                     //following lines set lane frequencies for 20M u4b operation. The center freuency for Zactkep (wspr 3) xmitions is hard set in WSPRBeacon.c to 14097100UL
		{
			case '1':XMIT_FREQUENCY=14097020UL;break;
			case '2':XMIT_FREQUENCY=14097060UL;break;
			case '3':XMIT_FREQUENCY=14097140UL;break;
			case '4':XMIT_FREQUENCY=14097180UL;break;
			default: XMIT_FREQUENCY=14097100UL;        //in case an invalid lane was read from EEPROM
		}	
   
	 WSPRbeaconContext *pWB = WSPRbeaconInit(
        _callsign,/** the Callsign. */
        CONFIG_LOCATOR4,/**< the default QTH locator if GPS isn't used. */
        10,             /**< Tx power, dbm. */
        &DCO,           /**< the PioDCO object. */
        XMIT_FREQUENCY,
        0,           /**< the carrier freq. shift relative to dial freq. */ //not used
        RFOUT_PIN,       /**< RF output GPIO pin. */
		(uint8_t)_start_minute[0]-'0',   /**< convert ASCI digits to ints  */
		(uint8_t)_id13[0]-'0',   
		(uint8_t)_suffix[0]-'0',
		_TELEN_config		
        );
    assert_(pWB);
    pWSPR = pWB;  //this lets things outside this routine access the WB context
    pWB->_txSched.force_xmit_for_testing = force_transmit;
	pWB->_txSched.led_mode = 0;  //0 means no serial comms from  GPS (critical fault if it remains that way)
	pWB->_txSched.verbosity=(uint8_t)_verbosity[0]-'0';       /**< convert ASCI digit to int  */
	pWB->_txSched.suffix=(uint8_t)_suffix[0]-'0';    /**< convert ASCI digit to int (value 253 if dash was entered) */
	pWB->_txSched.oscillatorOff=(uint8_t)_oscillator[0]-'0';
	pWB->_txSched.low_power_mode=(uint8_t)_battery_mode[0]-'0';
	strcpy(pWB->_txSched.id13,_id13);

	multicore_launch_core1(Core1Entry);    //caused immeditae reboot, so did GPS init
    StampPrintf("RF oscillator initialized.");
	int uart_number=(uint8_t)_custom_PCB[0]-'0';  //custom PCB uses Uart 1 if selected, otherwise uart 0
	DCO._pGPStime = GPStimeInit(uart_number, 9600, GPS_PPS_PIN, PLL_SYS_MHZ); //the 0 defines uart0, so the RX is GPIO 1 (pin 2 on pico). TX to GPS module not needed
    assert_(DCO._pGPStime);
	DCO._pGPStime->user_setup_menu_active=0;
	DCO._pGPStime->forced_XMIT_on=force_transmit;
	DCO._pGPStime->verbosity=(uint8_t)_verbosity[0]-'0';   
    int tick = 0;int tick2 = 0;  //used for timing various messages
	LED_sequence_start_time = get_absolute_time();

    for(;;)   //loop every ~ half second
    {		
		onewire_read();
		I2C_read();
		
		if(WSPRbeaconIsGPSsolutionActive(pWB))
        {
            const char *pgps_qth = WSPRbeaconGetLastQTHLocator(pWB);  //GET MAIDENHEAD       - this code in original fork wasnt working due to error in WSPRbeacon.c
            if(pgps_qth)
            {
                strncpy(pWB->_pu8_locator, pgps_qth, 6);     //does full 6 char maidenhead 				
//		        strcpy(pWB->_pu8_locator,"AA1ABC");          //DEBUGGING TO FORCE LOCATOR VALUE				
            }
        }        
        WSPRbeaconTxScheduler(pWB, YES, GPS_PPS_PIN);   
                
		if (pWB->_txSched.verbosity>=5)
		{
				if(0 == ++tick % 20)      //every ~20 secs dumps context.  
				 WSPRbeaconDumpContext(pWB);
		}	

		if (getchar_timeout_us(0)>0)   //looks for input on USB serial port only. Note: getchar_timeout_us(0) returns a -2 (as of sdk 2) if no keypress. But if you force it into a Char type, becomes something else
		{
		DCO._pGPStime->user_setup_menu_active=1;	
		user_interface();   
		}

		const float conversionFactor = 3.3f / (1 << 12);          //read temperature
		adc_select_input(4);	
		float adc = (float)adc_read() * conversionFactor;
		float tempC = 27.0f - (adc - 0.706f) / 0.001721f;		
		if (tempC < -50) { tempC  += 89; }			          //wrap around for overflow, per U4B protocol
		if (tempC > 39) { tempC  -= 89; }
		pWB->_txSched.temp_in_Celsius=tempC;           
		DCO._pGPStime->temp_in_Celsius=tempC;
		
		adc_select_input(3);  //if setup correctly, ADC3 reads Vsys   // read voltage
		float volts = 3*(float)adc_read() * conversionFactor;         //times 3 because of onboard voltage divider
			if (volts < 3.00) { volts += 1.95; }			          //wrap around for overflow, per U4B protocol
			if (volts > 4.95) { volts -= 1.95; }
		pWB->_txSched.voltage=volts;

 		process_TELEN_data();                          //if needed, this puts data into TELEN variables. You can remove this and set the data yourself as shown in the next two lines
		//pWB->_txSched.TELEN1_val1=rand() % 630000;   //the values  in TELEN_val1 and TELEN_val2 will get sent as TELEN #1 (extended Telemetry) (a third packet in the U4B protocol)
		//pWB->_txSched.TELEN1_val2=rand() % 153000;	/ max values are 630k and 153k
		
		if (pWB->_txSched.verbosity>=1)
		{
				if(0 == ++tick2 % 10)      //every ~5 sec
				StampPrintf("Temp: %0.1f  Volts: %0.1f  Altitude: %0.0f  Satellite count: %d\n", tempU,volts,DCO._pGPStime->_altitude ,DCO._pGPStime->_time_data.sat_count);		
				printf("TELEN Vals 1 through 4:  %d %d %d %d\n",telen_values[0],telen_values[1],telen_values[2],telen_values[3]);
		}
		
		for (int i=0;i < 10;i++) //orig code had a 900mS pause here. I only pause a total of 500ms, and spend it polling the time to handle LED state
			{
				handle_LED(pWB->_txSched.led_mode); 
				sleep_ms(50); 
			}
		DoLogPrint(); 	
	}
}
///////////////////////////////////

void process_TELEN_data(void)
{
		const float conversionFactor = 3300.0f / (1 << 12);   //3.3 * 1000. the 3.3 is from vref, the 1000 is to convert to mV. the 12 bit shift is because thats resolution of ADC

		for (int i=0;i < 4;i++)
		{			
		   switch(_TELEN_config[i])
			{
				case '-':  break; //do nothing, telen chan is disabled
				case '0': adc_select_input(0); telen_values[i] = round((float)adc_read() * conversionFactor);  		  break;
				case '1': adc_select_input(1); telen_values[i] = round((float)adc_read() * conversionFactor);  		  break;
				case '2': adc_select_input(2); telen_values[i] = round((float)adc_read() * conversionFactor); 		  break;
				case '3': adc_select_input(3); telen_values[i] = round((float)adc_read() * conversionFactor * 3.0f);  break;  //since ADC3 is hardwired to Battery via 3:1 voltage devider, make the conversion here
				case '4':					   telen_values[i] = pWSPR->_txSched.minutes_since_boot;   				  break; 			
				case '5': 	 				   telen_values[i] = pWSPR->_txSched.minutes_since_GPS_aquisition;		  break;			
				case '6': 	if (onewire_values[_TELEN_config[i]-'6']>0)     telen_values[i] = onewire_values[_TELEN_config[i]-'6']*100; else	telen_values[i] = 20000 + (-1*onewire_values[_TELEN_config[i]-'6'])*100;	  break;	
				case '7': 	if (onewire_values[_TELEN_config[i]-'6']>0)     telen_values[i] = onewire_values[_TELEN_config[i]-'6']*100; else	telen_values[i] = 20000 + (-1*onewire_values[_TELEN_config[i]-'6'])*100;	  break;	
				case '8': 	if (onewire_values[_TELEN_config[i]-'6']>0)     telen_values[i] = onewire_values[_TELEN_config[i]-'6']*100; else	telen_values[i] = 20000 + (-1*onewire_values[_TELEN_config[i]-'6'])*100;	  break;	
				case '9': 	if (onewire_values[_TELEN_config[i]-'6']>0)     telen_values[i] = onewire_values[_TELEN_config[i]-'6']*100; else	telen_values[i] = 20000 + (-1*onewire_values[_TELEN_config[i]-'6'])*100;	  break;				
			}	
		}

//onewire_values		
		pWSPR->_txSched.TELEN1_val1=telen_values[0];   // will get sent as TELEN #1 (extended Telemetry) (a third packet in the U4B protocol)
		pWSPR->_txSched.TELEN1_val2=telen_values[1];	// max values are 630k and 153k for val and val2
		pWSPR->_txSched.TELEN2_val1=telen_values[2];   //will get sent as TELEN #2 (extended Telemetry) (a 4th packet in the U4B protocol)
		pWSPR->_txSched.TELEN2_val2=telen_values[3];	// max values are 630k and 153k for val and val2

}

//////////////////////////////////////////////////////////////////////////////////////////////////////
void handle_LED(int led_state)
/**
 * @brief Handles setting LED to display mode.
 * 
 * @param led_state 1,2,3 or 4 to indicate the number of LED pulses. 0 is a special case indicating serial comm failure to GPS
 */
 			//////////////////////// LED HANDLING /////////////////////////////////////////////////////////
			
			/*
			LED MODE:
				0 - no serial comms to GPS module
				1 - No valid GPS, not transmitting
				2 - Valid GPS, waiting for time to transmitt
				3 - Valid GPS, transmitting
				4 - no valid GPS, but (still) transmitting anyway
			x rapid pulses to indicate mode, followed by pause. 0 is special case, continous rapid blink
			*/

{
 static int tik;
 uint64_t t = absolute_time_diff_us(LED_sequence_start_time, get_absolute_time());
 int i = t / 400000ULL;     //400mS total period of a LED flash

  if (led_state==0) 						//special case indicating serial comm failure to GPS. blink as rapidly as possible 
		  {
			if(0 == ++tik % 2) gpio_put(LED_PIN, 1); else gpio_put(LED_PIN, 0);     //very rapid
		  }
  else
  {
		  if (i<(led_state+1))
				{
				 if(t -(i*400000ULL) < 50000ULL)           //400mS total period of a LED flash, 50mS on pulse duration
							gpio_put(LED_PIN, 1);
				 else 
							gpio_put(LED_PIN, 0);
				}
		  if (t > 2500000ULL) 	LED_sequence_start_time = get_absolute_time();     //resets every 2.5 secs (total repeat length of led sequence).
  }
}
/////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Prints out hex listing of the settings NVRAM to stdio
 * 
 * @param buf Address of NVRAM to list
 * @param len Length of storage to list
 */
void print_buf(const uint8_t *buf, size_t len) {	

	printf(CLEAR_SCREEN);printf(BRIGHT);
	printf(BOLD_ON);printf(UNDERLINE_ON);
	printf("\nNVRAM dump: \n");printf(BOLD_OFF); printf(UNDERLINE_OFF);
 for (size_t i = 0; i < len; ++i) {
        printf("%02x", buf[i]);
        if (i % 16 == 15)
            printf("\n");
        else
            printf(" ");
    }
	printf(NORMAL);
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void display_intro(void)
{
printf(CLEAR_SCREEN);
printf(CURSOR_HOME);
printf(BRIGHT);
printf("\n\n\n\n\n\n\n\n\n\n\n\n");
printf("================================================================================\n\n");printf(UNDERLINE_ON);
printf("Pico-WSPRer (pico whisper-er) by KC3LBR,  version: %s %s\n\n",__DATE__ ,__TIME__);printf(UNDERLINE_OFF);
printf("Instructions and source: https://github.com/EngineerGuy314/pico-WSPRer\n");
printf("Forked from: https://github.com/RPiks/pico-WSPR-tx\n");
printf("Additional functions, fixes and documention by https://github.com/serych\n\n");
printf("Consult https://traquito.github.io/channelmap/ to find an open channel \nand make note of id13 (column headers), minute and lane (frequency)\n");
printf("\n================================================================================\n");

printf(RED);printf("press anykey to continue");printf(NORMAL); 
char c=getchar_timeout_us(60000000);	//wait 
printf(CLEAR_SCREEN);
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////
void show_TELEN_msg()
{
printf(BRIGHT);
printf("\n\n\n\n");printf(UNDERLINE_ON);
printf("TELEN CONFIG INSTRUCTIONS:\n\n");printf(UNDERLINE_OFF);
printf(NORMAL); 
printf("* There are 4 possible TELEN values, corresponding to TELEN 1 value 1,\n");
printf("  TELEN 1 value 2, TELEN 2 value 1 and TELEN 2 value 2.\n");
printf("* Enter 4 characters in TELEN_config. use a '-' (minus) to disable one \n");
printf("  or more values.\n* example: '----' disables all telen \n");
printf("* example: '01--' sets Telen 1 value 1 to type 0, \n  Telen 1 val 2 to type 1,  disables all of TELEN 2 \n"); printf(BRIGHT);printf(UNDERLINE_ON);
printf("\nTelen Types:\n\n");printf(UNDERLINE_OFF);printf(NORMAL); 
printf("-: disabled, 0: ADC0, 1: ADC1, 2: ADC2, 3: ADC3,\n");
printf("4: minutes since boot, 5: minutes since GPS fix aquired \n");
printf("6-9: OneWire temperature sensors 1 though 4 \n");
printf("A: custom: OneWire temperature sensor 1 hourly low/high \n");
printf("B-Z: reserved for Future: I2C devices, other modes etc \n");
printf("\n(ADC values come through in units of mV)\n");
printf("See the Wiki for more info.\n\n");
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Function that implements simple user interface via UART
 * 
 * For every new config variable to be added to the interface:
	1: create a global character array at top of main.c 
	2: add entry in read_NVRAM()
	3: add entry in write_NVRAM()
	4: add limit checking in check_data_validity()
	5: add limit checking in check_data_validity_and_set_defaults()
	6: add TWO entries in show_values() (to display name and value, and also to display which key is used to change it)
	7: add CASE statement entry in user_interface()
	8: Either do something with the variable locally in Main.c, or if needed elsewhere:
		-- add a member to the GPStimeContext or WSPRbeaconContext structure
		-- add code in main.c to move the data from the local _tag to the context structure
		-- do something with the data elsewhere in the program
 */
void user_interface(void)                                //called if keystroke from terminal on USB detected during operation.
{
int c;
char str[10];

gpio_put(GPS_ENABLE_PIN, 0);                   //shutoff gps to prevent serial input  (probably not needed anymore)
sleep_ms(100);
gpio_put(LED_PIN, 1); //LED on.	

display_intro();
show_values();          /* shows current VALUES  AND list of Valid Commands */

    for(;;)
	{	
																 printf(UNDERLINE_ON);printf(BRIGHT);
		printf("\nEnter the command (X,C,S,I,M,L,V,O,P,T,F):");printf(UNDERLINE_OFF);printf(NORMAL);	
		c=getchar_timeout_us(60000000);		   //just in case user setup menu was enterred during flight, this will reboot after 60 secs
		printf("%c\n", c);
		if (c==PICO_ERROR_TIMEOUT) {printf(CLEAR_SCREEN);printf("\n\n TIMEOUT WAITING FOR INPUT, REBOOTING FOR YOUR OWN GOOD!\n");sleep_ms(100);watchdog_enable(100, 1);for(;;)	{}}
		if (c>90) c-=32; //make it capital either way
		switch(c)
		{
			case 'X':printf(CLEAR_SCREEN);printf("\n\nGOODBYE");watchdog_enable(100, 1);for(;;)	{}
			//case 'R':printf(CLEAR_SCREEN);printf("\n\nCorrupting data..");strncpy(_callsign,"!^&*(",6);write_NVRAM();watchdog_enable(100, 1);for(;;)	{}  //used for testing NVRAM check on boot feature
			case 'C':get_user_input("Enter callsign: ",_callsign,sizeof(_callsign)); convertToUpperCase(_callsign); write_NVRAM(); break;
			case 'S':get_user_input("Enter single digit numeric suffix: ", _suffix, sizeof(_suffix)); convertToUpperCase(_suffix); write_NVRAM(); break;
			case 'I':get_user_input("Enter id13: ", _id13,sizeof(_id13)); convertToUpperCase(_id13); write_NVRAM(); break;
			case 'M':get_user_input("Enter starting Minute: ", _start_minute, sizeof(_start_minute)); write_NVRAM(); break;
			case 'L':get_user_input("Enter Lane (1,2,3,4): ", _lane, sizeof(_lane)); write_NVRAM(); break;
			case 'V':get_user_input("Verbosity level (0-9): ", _verbosity, sizeof(_verbosity)); write_NVRAM(); break;
			case 'O':get_user_input("Oscillator off (0,1): ", _oscillator, sizeof(_oscillator)); write_NVRAM(); break;
			case 'P':get_user_input("custom Pcb mode (0,1): ", _custom_PCB, sizeof(_custom_PCB)); write_NVRAM(); break;
			case 'T':show_TELEN_msg();get_user_input("TELEN config: ", _TELEN_config, sizeof(_TELEN_config)); convertToUpperCase(_TELEN_config); write_NVRAM(); break;
			case 'B':get_user_input("Battery mode (0,1): ", _battery_mode, sizeof(_battery_mode)); write_NVRAM(); break;
			case 'F':
				printf("Fixed Frequency output (antenna tuning mode). Enter frequency (for example 14.097) or 0 for exit.\n\t");
				char _tuning_freq[7];
				float frequency;
				while(1)
				{
					get_user_input("Frequency to generate (MHz): ", _tuning_freq, sizeof(_tuning_freq));  //blocking until next input
					frequency = atof(_tuning_freq);
					if (!frequency) {break;}
					printf("Generating %.3f MHz\n", frequency);
					pWSPR->_pTX->_u32_dialfreqhz = (uint32_t)(frequency * MHZ);
					pWSPR->_txSched.force_xmit_for_testing = YES;
					return;  // returns to main loop
				}
			case 13:  break;
			case 10:  break;
			default: printf(CLEAR_SCREEN); printf("\nYou pressed: %c - (0x%02x), INVALID choice!! ",c,c);sleep_ms(2000);break;		
		}
		check_data_validity_and_set_defaults();
		show_values();
	}
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Reads part of the program memory where the user settings are saved
 * prints hexa listing of data and calls function which check data validity
 * 
 */
void read_NVRAM(void)
{
const uint8_t *flash_target_contents = (const uint8_t *) (XIP_BASE + FLASH_TARGET_OFFSET); //a pointer to a safe place after the program memory

print_buf(flash_target_contents, FLASH_PAGE_SIZE); //256

strncpy(_callsign, flash_target_contents, 6);
strncpy(_id13, flash_target_contents+6, 2);
strncpy(_start_minute, flash_target_contents+8, 1);
strncpy(_lane, flash_target_contents+9, 1);
strncpy(_suffix, flash_target_contents+10, 1);
strncpy(_verbosity, flash_target_contents+11, 1);
strncpy(_oscillator, flash_target_contents+12, 1);
strncpy(_custom_PCB, flash_target_contents+13, 1);
strncpy(_TELEN_config, flash_target_contents+14, 4);
strncpy(_battery_mode, flash_target_contents+18, 1);
 
}
/////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Writes the user entered data into NVRAM
 * 
 */
void write_NVRAM(void)
{
    uint8_t data_chunk[FLASH_PAGE_SIZE];

	strncpy(data_chunk,_callsign, 6);
	strncpy(data_chunk+6,_id13,  2);
	strncpy(data_chunk+8,_start_minute, 1);
	strncpy(data_chunk+9,_lane, 1);
	strncpy(data_chunk+10,_suffix, 1);
	strncpy(data_chunk+11,_verbosity, 1);
	strncpy(data_chunk+12,_oscillator, 1);
	strncpy(data_chunk+13,_custom_PCB, 1);
	strncpy(data_chunk+14,_TELEN_config, 4);
	strncpy(data_chunk+18,_battery_mode, 1);

	uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
	flash_range_program(FLASH_TARGET_OFFSET, data_chunk, FLASH_PAGE_SIZE);
	restore_interrupts (ints);

}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Checks validity of user settings and if something is wrong, it sets "factory defaults"
 * and writes it back to NVRAM
 * 
 */
void check_data_validity_and_set_defaults(void)
{
//do some basic plausibility checking on data, set reasonable defaults if memory was uninitialized							
	if ( ((_callsign[0]<'A') || (_callsign[0]>'Z')) && ((_callsign[0]<'0') || (_callsign[0]>'9'))    ) {   strncpy(_callsign,"AB1CDE",6);     ; write_NVRAM();} 
	if ( ((_suffix[0]<'0') || (_suffix[0]>'9')) && (_suffix[0]!='X') ) {_suffix[0]='-'; write_NVRAM();} //by default, disable zachtek suffix
	if ( (_id13[0]!='0') && (_id13[0]!='1') && (_id13[0]!='Q')&& (_id13[0]!='-')) {strncpy(_id13,"Q0",2); write_NVRAM();}
	if ( (_start_minute[0]!='0') && (_start_minute[0]!='2') && (_start_minute[0]!='4')&& (_start_minute[0]!='6')&& (_start_minute[0]!='8')) {_start_minute[0]='0'; write_NVRAM();}
	if ( (_lane[0]!='1') && (_lane[0]!='2') && (_lane[0]!='3')&& (_lane[0]!='4')) {_lane[0]='2'; write_NVRAM();}
	if ( (_verbosity[0]<'0') || (_verbosity[0]>'9')) {_verbosity[0]='1'; write_NVRAM();} //set default verbosity to 1
	if ( (_oscillator[0]<'0') || (_oscillator[0]>'1')) {_oscillator[0]='1'; write_NVRAM();} //set default oscillator to switch off after the trasmission
	if ( (_custom_PCB[0]<'0') || (_custom_PCB[0]>'1')) {_custom_PCB[0]='0'; write_NVRAM();} //set default IO mapping to original Pi Pico configuration
	if ( (_TELEN_config[0]<'0') || (_TELEN_config[0]>'F')) {strncpy(_TELEN_config,"----",4); write_NVRAM();}
	if ( (_battery_mode[0]<'0') || (_battery_mode[0]>'1')) {_battery_mode[0]='0'; write_NVRAM();} //
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Checks validity of user settings and returns -1 if something wrong. Does NOT set defaults or alter NVRAM.
 * 
 */
int check_data_validity(void)
{
int result=1;	
//do some basic plausibility checking on data				
	if ( ((_callsign[0]<'A') || (_callsign[0]>'Z')) && ((_callsign[0]<'0') || (_callsign[0]>'9'))    ) {result=-1;} 
	if ( ((_suffix[0]<'0') || (_suffix[0]>'9')) && (_suffix[0]!='-') && (_suffix[0]!='X') ) {result=-1;} 
	if ( (_id13[0]!='0') && (_id13[0]!='1') && (_id13[0]!='Q')&& (_id13[0]!='-')) {result=-1;}
	if ( (_start_minute[0]!='0') && (_start_minute[0]!='2') && (_start_minute[0]!='4')&& (_start_minute[0]!='6')&& (_start_minute[0]!='8')) {result=-1;}
	if ( (_lane[0]!='1') && (_lane[0]!='2') && (_lane[0]!='3')&& (_lane[0]!='4')) {result=-1;}
	if ( (_verbosity[0]<'0') || (_verbosity[0]>'9')) {result=-1;} 
	if ( (_oscillator[0]<'0') || (_oscillator[0]>'1')) {result=-1;} 
	if ( (_custom_PCB[0]<'0') || (_custom_PCB[0]>'1')) {result=-1;} 
	if ( ((_TELEN_config[0]<'0') || (_TELEN_config[0]>'F'))&& (_TELEN_config[0]!='-')) {result=-1;}
	if ( (_battery_mode[0]<'0') || (_battery_mode[0]>'1')) {result=-1;} 	
return result;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * @brief Function that writes out the current set values of parameters
 * 
 */
void show_values(void) /* shows current VALUES  AND list of Valid Commands */
{
								printf(CLEAR_SCREEN);printf(UNDERLINE_ON);printf(BRIGHT);
printf("\n\nCurrent values:\n");printf(UNDERLINE_OFF);printf(NORMAL);

printf("\n\tCallsign:%s\n\t",_callsign);
printf("Suffix:%s\n\t",_suffix);
printf("Id13:%s\n\t",_id13);
printf("Minute:%s\n\t",_start_minute);
printf("Lane:%s\n\t",_lane);
printf("Verbosity:%s\n\t",_verbosity);
printf("Oscillator Off:%s\n\t",_oscillator);
printf("custom Pcb IO mappings:%s\n\t",_custom_PCB);
printf("TELEN config:%s\n\t",_TELEN_config);
printf("Battery (low power) mode:%s\n\n",_battery_mode);

							printf(UNDERLINE_ON);printf(BRIGHT);
printf("VALID commands: ");printf(UNDERLINE_OFF);printf(NORMAL);

printf("\n\n\tX: eXit configuration and reboot\n\tC: change Callsign (6 char max)\n\t");
printf("S: change Suffix (added to callsign for WSPR3) use '-' to disable WSPR3\n\t");
printf("I: change Id13 (two alpha numeric chars, ie Q8) use '--' to disable U4B\n\t");
printf("M: change starting Minute (0,2,4,6,8)\n\tL: Lane (1,2,3,4) corresponding to 4 frequencies in 20M band\n\t");
printf("V: Verbosity level (0 for no messages, 9 for too many) \n\t");
printf("O: Oscillator off after trasmission (default: 1) \n\t");
printf("P: custom Pcb mode IO mappings (0,1)\n\t");
printf("T: TELEN config\n\t");
printf("B: Battery (low power) mode \n\t");
printf("F: Frequency output (antenna tuning mode)\n\n");


}
/**
 * @brief Converts string to upper case
 * 
 * @param str string to convert
 * @return No return value, string is converted directly in the parameter *str  
 */
void convertToUpperCase(char *str) {
    while (*str) {
        *str = toupper((unsigned char)*str);
        str++;
    }
}
/**
 * @brief Initializes Pico pins
 * 
 */
void InitPicoPins(void)
{
/*  gpio_init(18); 
	gpio_set_dir(18, GPIO_OUT); //GPIO 18 used for fan control when testing TCXO stability */

	int use_custom_PCB_mappings=(uint8_t)_custom_PCB[0]-'0'; 
	if (use_custom_PCB_mappings==0)                            //do not use parallel IO low-side drive if using custom PCB
	{		
	GPS_PPS_PIN = GPS_PPS_PIN_default;
	RFOUT_PIN = RFOUT_PIN_default;
	GPS_ENABLE_PIN = GPS_ENABLE_PIN_default;
	gpio_init(GPS_ENABLE_PIN); gpio_set_dir(GPS_ENABLE_PIN, GPIO_OUT); //initialize GPS enable output 
	gpio_put(GPS_ENABLE_PIN, 1);   //turn on output to enable the MOSFET
	gpio_init(GPS_ALT_ENABLE_LOW_SIDE_DRIVE_BASE_IO_PIN); gpio_set_dir(GPS_ALT_ENABLE_LOW_SIDE_DRIVE_BASE_IO_PIN, GPIO_OUT); //alternate way to enable the GPS is to pull down its ground (aka low-side drive) using 3 GPIO in parallel (no mosfet needed). 2 do: make these non-hardcoded
	gpio_init(GPS_ALT_ENABLE_LOW_SIDE_DRIVE_BASE_IO_PIN+1); gpio_set_dir(GPS_ALT_ENABLE_LOW_SIDE_DRIVE_BASE_IO_PIN+1, GPIO_OUT); //no need to actually write a value to these outputs. Just enabling them as outputs is fine, they default to the off state when this is done. perhaps thats a dangerous assumption? 
	gpio_init(GPS_ALT_ENABLE_LOW_SIDE_DRIVE_BASE_IO_PIN+2); gpio_set_dir(GPS_ALT_ENABLE_LOW_SIDE_DRIVE_BASE_IO_PIN+2, GPIO_OUT);
	gpio_for_onewire=ONEWIRE_bus_pin;
	}
	
		else                          //if using custom PCB 
		{	
			gpio_for_onewire=ONEWIRE_bus_pin_pcb;
			GPS_PPS_PIN = GPS_PPS_PIN_pcb;
			RFOUT_PIN = RFOUT_PIN_pcb;
			GPS_ENABLE_PIN = GPS_ENABLE_PIN_pcb;
			gpio_init(GPS_ENABLE_PIN); gpio_set_dir(GPS_ENABLE_PIN, GPIO_OUT); //initialize GPS enable output (INVERSE LOGIC on custom PCB, so just initialize it, leave it at zero state)	

			gpio_init(6); gpio_set_dir(6, GPIO_OUT);gpio_put(6, 1); //these are required ONLY for v0.1 of custom PCB (ON/OFF and nReset of GPS module, which later are just left disconnected)
			gpio_init(5); gpio_set_dir(5, GPIO_OUT);gpio_put(5, 1); //these are required ONLY for v0.1 of custom PCB (ON/OFF and nReset of GPS module, which later are just left disconnected)

	}

	dallas_setup();  //configures one-wire interface. Enabled pullup on one-wire gpio. must do this here, in case they want to use analog instead, because then pullup needs to be disabled below.

	for (int i=0;i < 4;i++)   //init ADC(s) as needed for TELEN
		{			
		   switch(_TELEN_config[i])
			{
				case '-':  break; //do nothing, telen chan is disabled
				case '0': gpio_init(26);gpio_set_dir(26, GPIO_IN);gpio_set_pulls(26,0,0);break;
				case '1': gpio_init(27);gpio_set_dir(27, GPIO_IN);gpio_set_pulls(27,0,0);break;
				case '2': gpio_init(28);gpio_set_dir(28, GPIO_IN);gpio_set_pulls(28,0,0);break; 
			}
			
		}
	
	gpio_init(PICO_VSYS_PIN);  		//Prepare ADC 3 to read Vsys
	gpio_set_dir(PICO_VSYS_PIN, GPIO_IN);
	gpio_set_pulls(PICO_VSYS_PIN,0,0);
    adc_init();
    adc_set_temp_sensor_enabled(true); 	//Enable the onboard temperature sensor


    // RF pins are initialised in /hf-oscillator/dco2.pio. Here is only pads setting
    // trying to set the power of RF pads to maximum and slew rate to fast (Chapter 2.19.6.3. Pad Control - User Bank in the RP2040 datasheet)
    // possible values: PADS_BANK0_GPIO0_DRIVE_VALUE_12MA, ..._8MA, ..._4MA, ..._2MA
    // values of constants are the same for all the pins, so doesn't matter if we use PADS_BANK0_GPIO6_DRIVE_VALUE_12MA or ..._GPIO0_DRIVE...
    /*  Measurements have shown that the drive value and slew rate settings do not affect the output power. Therefore, the lines are commented out.
    hw_write_masked(&padsbank0_hw->io[RFOUT_PIN],
                (PADS_BANK0_GPIO0_DRIVE_VALUE_12MA << PADS_BANK0_GPIO0_DRIVE_LSB) || PADS_BANK0_GPIO0_SLEWFAST_FAST,
                PADS_BANK0_GPIO0_DRIVE_BITS || PADS_BANK0_GPIO0_SLEWFAST_BITS);           // first RF pin 
    hw_write_masked(&padsbank0_hw->io[RFOUT_PIN+1],
                (PADS_BANK0_GPIO0_DRIVE_VALUE_12MA << PADS_BANK0_GPIO0_DRIVE_LSB) || PADS_BANK0_GPIO0_SLEWFAST_FAST,
                PADS_BANK0_GPIO0_DRIVE_BITS || PADS_BANK0_GPIO0_SLEWFAST_BITS);           // second RF pin
    */            

}

void I2C_init(void)   //this was used for testing HMC5883L compass module. keeping it here as a template for future I2C use
{
/*		
    i2c_init(i2c_default, 100 * 1000);
    gpio_set_function(20, GPIO_FUNC_I2C);    //pins 20 and 21 for original Pi PIco  (20 Data, 21 Clk) , Custom PCB will use gpio 0,1 instead
    gpio_set_function(21, GPIO_FUNC_I2C);
    gpio_pull_up(20);
    gpio_pull_up(21);

	uint8_t i2c_buf[6];
    uint8_t config_buf[2];
	uint8_t write_config_buf[2];
	uint8_t reg;
	#define ADDR _u(0x1E)   //address of compass module

    config_buf[0] = 0x00; //config register A	
    config_buf[1] =0b00100;  //1.5Hz max update rate
    i2c_write_blocking(i2c_default, ADDR, config_buf, 2, false);
    config_buf[0] = 0x01; //config register B	
    config_buf[1] =0b00000000;  //max gain
    i2c_write_blocking(i2c_default, ADDR, config_buf, 2, false);
    config_buf[0] = 0x02; //Mode register
    config_buf[1] =0x00;  //normal mode
    i2c_write_blocking(i2c_default, ADDR, config_buf, 2, false);
    printf("Done I2C config \n");
*/
}
void I2C_read(void)  //this was used for testing HMC5883L compass module. keeping it here as a template for future I2C use
{
	/*
	write_config_buf[0]=0x3;  											//reg number to start reading at
	i2c_write_blocking(i2c_default, ADDR, write_config_buf , 1, true);  // send 3 to tell it we about to READ from register 3, and keep Bus control true
    i2c_read_blocking(i2c_default, ADDR, i2c_buf, 6, false);            //reads six bytes of registers, starting at address you used above
	int16_t x_result = (int16_t)((i2c_buf[0]<<8)|i2c_buf[1]);           //not bothering with Z axis, because assume sensor board is horizontal
	int16_t y_result = (int16_t)((i2c_buf[4]<<8)|i2c_buf[5]);
	printf("X: %d\n Y: %d\n",x_result,y_result);    //to make a useful "compass", you would need to keep track of max/min X,y values, scale them against those limits, take ratio of the two scaled values, and that corresponds to heading. direction (to direction)
	*/
}

void onewire_read()
{
                if ((ow_read(&one_wire_interface) != 0)&&(number_of_onewire_devs>0))   //if conversions ready, read it
				{
                // read the result from each device                   
                for (int i = 0; i < number_of_onewire_devs; i += 1) 
					{				
						ow_reset (&one_wire_interface);
						ow_send (&one_wire_interface, OW_MATCH_ROM);
							for (int b = 0; b < 64; b += 8) {
								ow_send (&one_wire_interface, OW_romcodes[i] >> b);
							   }
						ow_send (&one_wire_interface, DS18B20_READ_SCRATCHPAD);
						int16_t temp = 0;
						temp = ow_read (&one_wire_interface) | (ow_read (&one_wire_interface) << 8);
						if (temp!=-1)
						onewire_values[i]= 32.0 + ((temp / 16.0)*1.8);
						else printf("\nOneWire device read failure!! re-using previous value\n");
						//printf ("\t%d: %f", i,onewire_values[i]);
					}
					  // start temperature conversion in parallel on all devices so they will be ready for the next time i try to read them
					  // (see ds18b20 datasheet)
					  ow_reset (&one_wire_interface);
					  ow_send (&one_wire_interface, OW_SKIP_ROM);
					  ow_send (&one_wire_interface, DS18B20_CONVERT_T);
				}

}
//sets up OneWire interface
void dallas_setup() {  

    PIO pio = pio0;
    uint offset;
	gpio_init(gpio_for_onewire);
	gpio_pull_up(gpio_for_onewire);  //with this you dont need external pull up resistor on data line (phantom power still won't work though)

    // add the program to the PIO shared address space
    if (pio_can_add_program (pio, &onewire_program)) {
        offset = pio_add_program (pio, &onewire_program);
		
		if (ow_init (&one_wire_interface, pio, offset, gpio_for_onewire))  // claim a state machine and initialise a driver instance
		 {
            // find and display 64-bit device addresses

            number_of_onewire_devs = ow_romsearch (&one_wire_interface, OW_romcodes, maxdevs, OW_SEARCH_ROM);

            printf("Found %d devices\n", number_of_onewire_devs);      
            for (int i = 0; i < number_of_onewire_devs; i += 1) {
                printf("\t%d: 0x%llx\n", i, OW_romcodes[i]);
            }
            putchar ('\n');
         
		  // start temperature conversion in parallel on all devices right now so the values will be ready to read as soon as i try to
          // (see ds18b20 datasheet)
          ow_reset (&one_wire_interface);
          ow_send (&one_wire_interface, OW_SKIP_ROM);
          ow_send (&one_wire_interface, DS18B20_CONVERT_T);

		} else	puts ("could not initialise the onewire driver");
     }
}
/**
* @note:
* Verbosity notes:
* 0: none
* 1: temp/volts every second, message if no gps
* 2: GPS status every second
* 3:          messages when a xmition started
* 4: x-tended messages when a xmition started 
* 5: dump context every 20 secs
* 6: show PPB every second
* 7: Display GxRMC and GxGGA messages
* 8: display ALL serial input from GPS module
*/

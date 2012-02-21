////////////////////////////////////////////////////////////////////////////
// Brad Goold 2012 
// DS1820 driver for STM32F103VE using FreeRTOS
////////////////////////////////////////////////////////////////////////////

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f10x.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ds1820.h"
#include "queue.h"
#include "console.h"
#include "lcd.h"


// ROM COMMANDS
#define MATCH_ROM	0x55
#define	SEARCH_ROM	0xF0
#define SKIP_ROM	0xCC
#define READ_ROM        0x33
#define ALARM_SEARCH    0xEC

// FUNCTION COMMANDS
#define CONVERT_TEMP     0x44
#define COPY_SCRATCHPAD  0x48
#define WRITE_SCRATCHPAD 0x4E
#define READ_SCRATCHPAD  0xBE
#define RECALL_EEPROM    0xB8
#define READ_PS          0xB4

#define MAX_SENSORS      10

// ERROR DEFINES
#define BUS_ERROR      0xFE
#define PRESENCE_ERROR 0xFD
#define NO_ERROR       0x00

// BUS COMMANDS
#define DQ_IN()  {DS1820_PORT->CRH&=0xFFFFF0FF;DS1820_PORT->CRH |= 0x00000400;}
#define DQ_OUT() {DS1820_PORT->CRH&=0xFFFFF0FF;DS1820_PORT->CRH |= 0x00000300;}
#define DQ_SET()   GPIO_SetBits(DS1820_PORT, DS1820_PIN)
#define DQ_RESET() GPIO_ResetBits(DS1820_PORT, DS1820_PIN)
#define DQ_READ()  GPIO_ReadInputDataBit(DS1820_PORT, DS1820_PIN)

// STATIC FUNCTIONS
static void ds1820_convert(void);
static void ds1820_init(void);
static unsigned char ds1820_reset(void);
static void ds1820_write_bit(uint8_t bit);
static uint8_t ds1820_read_bit(void);
static void ds1820_write_byte(uint8_t byte);
static uint8_t ds1820_read_byte(void);
static void ds1820_convert(void);
static uint8_t ds1820_search();
static float ds1820_read_device(uint8_t * rom_code);


uint8_t sp[9]; //scratchpad
uint8_t sp1[9]; //scratchpad
int16_t ds1820_temperature = 0;

uint8_t rom[8];

static void delay_us(uint16_t count){ 
    
    uint16_t TIMCounter = count;
    TIM_Cmd(TIM2, ENABLE);
    TIM_SetCounter(TIM2, TIMCounter);
    while (TIMCounter)
    {
        TIMCounter = TIM_GetCounter(TIM2);
    }
    TIM_Cmd(TIM2, DISABLE);
}


#define HLT_TEMP_SENSOR "\x10\x9c\xa4\x1e\x02\x08\x00\xf"
#define MASH_TEMP_SENSOR "\x10\xe3\x9b\x1e\x02\x08\x00\x58"
#define CABINET_TEMP_SENSOR "\x10\xe3\x9b\x1e\x02\x08\x00\x58"
#define AMBIENT_TEMP_SENSOR "\x10\xe3\x9b\x1e\x02\x08\x00\x58"

char * b[5];

void vTaskDS1820Convert( void *pvParameters ){
    char buf[30];
    int ii = 0;
    float mash_temp, hlt_temp, cabinet_temp, ambient_temp; 

    // Allocate memory for sensors
    b[HLT] = (char *) malloc (sizeof(rom)+1);
    b[MASH] = (char *) malloc (sizeof(rom)+1);
    b[CABINET] = (char *) malloc (sizeof(rom)+1);
    b[AMBIENT] = (char *) malloc (sizeof(rom)+1);
    b[SPARE] = (char *) malloc (sizeof(rom)+1);
    
    // Copy default values
    memcpy(b[HLT], HLT_TEMP_SENSOR, sizeof(HLT_TEMP_SENSOR)+1);
    memcpy(b[MASH], MASH_TEMP_SENSOR, sizeof(MASH_TEMP_SENSOR)+1);
    memcpy(b[CABINET], CABINET_TEMP_SENSOR, sizeof(CABINET_TEMP_SENSOR)+1);
    memcpy(b[AMBIENT], AMBIENT_TEMP_SENSOR, sizeof(AMBIENT_TEMP_SENSOR)+1);  

    // initialise the bus
    ds1820_init();
    if (ds1820_reset() ==PRESENCE_ERROR)
    {
        sprintf(buf, "NO SENSOR DETECTED\r\n");
        xQueueSendToBack(xConsoleQueue, &buf, 1000);
        vTaskDelete(NULL); // if this task fails... delete it
    }
  
    
    for (;;)
    {
        ds1820_convert();
        vTaskDelay(1000);
        
        hlt_temp = ds1820_read_device(b[HLT]);
        mash_temp = ds1820_read_device(b[MASH]);
        cabinet_temp = ds1820_read_device(b[CABINET]);
        ambient_temp = ds1820_read_device(b[AMBIENT]);
        
        // Uncomment below to send temps to the console
        /*
        sprintf(buf, "HLT Temp = %.2fDeg-C\r\n", hlt_temp);
        xQueueSendToBack(xConsoleQueue, &buf, 100);
        sprintf(buf, "Mash Temp = %.2fDeg-C\r\n", mash_temp);
        xQueueSendToBack(xConsoleQueue, &buf, 100);
        sprintf(buf, "Cabinet Temp = %.2fDeg-C\r\n", cabinet_temp);
        xQueueSendToBack(xConsoleQueue, &buf, 100);
        sprintf(buf, "Ambient Temp = %.2fDeg-C\r\n", ambient_temp);
        xQueueSendToBack(xConsoleQueue, &buf, 100);
        */
        
        taskYIELD();
    }
    
}

static void ds1820_init(void) {
    // intialise timer 2 for counting 
    TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure;
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    TIM_TimeBaseStructure.TIM_Period = 1;                
    TIM_TimeBaseStructure.TIM_Prescaler = 72;       //72MHz->1MHz
    TIM_TimeBaseStructure.TIM_ClockDivision = 0;   
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Down;  
    TIM_TimeBaseInit(TIM2, &TIM_TimeBaseStructure);

    GPIO_InitTypeDef GPIO_InitStructure;
    
    // PC10-DQ
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);

    GPIO_InitStructure.GPIO_Pin =  GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
    //printf("ioMode = %x\r\n", GPIOC->CRH);


}




static unsigned char ds1820_reset(void)
{
    portENTER_CRITICAL();
    DQ_OUT();
    DQ_SET();
    DQ_RESET();
    delay_us(500);
    DQ_IN();
    
    delay_us(30);
    if (DQ_READ()!= 0)
    {
        portEXIT_CRITICAL();
        return PRESENCE_ERROR;
       
    }
    portEXIT_CRITICAL();
    DQ_OUT();
    DQ_SET();
    delay_us(200); 
    
    return NO_ERROR;

}


static void ds1820_write_bit(uint8_t bit){

    DQ_OUT();
    DQ_SET(); // make sure bus is high
    portENTER_CRITICAL();
    DQ_RESET(); // pull low for 2us
    delay_us(2);
    if (bit){
        DQ_IN();
    }
    else DQ_RESET();
    delay_us(60);
    portEXIT_CRITICAL();
    DQ_IN(); // return bus
}

static uint8_t ds1820_read_bit(void){
    uint8_t bit;
    delay_us(1);
    DQ_OUT();
    DQ_SET(); // make sure bus is high
    portENTER_CRITICAL();
    DQ_RESET(); // pull low for 2us
    delay_us(1);
    DQ_IN(); // give bus back to DS1820;
    delay_us(5); 
    bit = DQ_READ();
    delay_us(56);
    //DQ_OUT();
    //DQ_SET();
    portEXIT_CRITICAL();
    if (bit) 
        return 1;
    else return 0; 
}

static void ds1820_write_byte(uint8_t byte){

    delay_us(100);
    portENTER_CRITICAL();
    int ii;
    for (ii = 0; ii < 8; ii++) {
        if (byte&0x01){
            ds1820_write_bit(1);
            // printf("1 written\r\n");
        }
        else {
            ds1820_write_bit(0);
            //printf("0 written\r\n");
        }
        byte = byte>>1;
    } 
    //delay_us(100);
   portEXIT_CRITICAL();
}

static uint8_t ds1820_read_byte(void){
    int ii;
    uint8_t bit, byte = 0;
    portENTER_CRITICAL();
    delay_us(1);
    for (ii = 0; ii < 8; ii++) {
        byte = byte >> 1;
        bit = ds1820_read_bit();
        if (bit==0) {
            byte = byte & 0x7F;
        }
        else byte = byte | 0x80;
        
        
    }
     portEXIT_CRITICAL();
    return byte;
}

static void ds1820_convert(void){
    ds1820_reset();
    ds1820_write_byte(SKIP_ROM); 
    ds1820_write_byte(CONVERT_TEMP);
    DQ_IN();
       
}

float ds1820_get_temp(unsigned char sensor){

    return ds1820_read_device(b[sensor]);
}
                              
 
uint8_t ds1820_one_device_get_temp(void){
    int ii;

    ds1820_reset();
    ds1820_write_byte(SKIP_ROM); 
    ds1820_write_byte(READ_SCRATCHPAD);
    for (ii = 0; ii < 9; ii++){
        sp[ii] = ds1820_read_byte();
    }
    ds1820_reset();
    ds1820_temperature = sp[1] & 0x0f;
    ds1820_temperature <<= 8;
    ds1820_temperature |= sp[0];
    unsigned char remain = sp[6];
    ds1820_temperature >>= 1;
    ds1820_temperature = (ds1820_temperature * 100) -  25  + (100 * 16 - remain * 100) / (16);

    //ds1820_fahrenheit = 3200 + (ds1820_temperature * 9) / 5;
    
}

static uint8_t ds1820_search(){
    uint8_t ii;
    char console_text[30];
    // ds1820_reset();
    ds1820_reset();
    ds1820_write_byte(READ_ROM); 
    portENTER_CRITICAL();
    for (ii = 0; ii < 8; ii++){
        rom[ii] = ds1820_read_byte();
    }
    portEXIT_CRITICAL();
    ds1820_reset();
    sprintf(console_text, "Sensor search complete\r\n\0");
    xQueueSendToBack(xConsoleQueue, &console_text, 0);
    /*
    printf("DS1820 ROM CODE: MSB->"); 
    for (ii = 7; ii >= 0 ; ii--)
    {
        printf("%x", rom[ii]);
    }
    printf("<-LSB\r\n"); 
    */
}



static float ds1820_read_device(uint8_t * rom_code){
    float retval;
    uint16_t ds1820_temperature1 = 10000;
    uint8_t ii; 
    ds1820_reset();
    ds1820_write_byte(MATCH_ROM);
    portENTER_CRITICAL();
    for (ii = 0; ii < 8; ii++)
    {
        ds1820_write_byte(rom_code[ii]);
    }
    ds1820_write_byte(READ_SCRATCHPAD);
    for (ii = 0; ii < 9; ii++){
        sp1[ii] = ds1820_read_byte();
    }
    portEXIT_CRITICAL();
    ds1820_reset();
    ds1820_temperature1 = sp1[1] & 0x0f;
    ds1820_temperature1 <<= 8;
    ds1820_temperature1 |= sp1[0];
    unsigned char remain = sp1[6];
    ds1820_temperature1 >>= 1;
    ds1820_temperature1 = (ds1820_temperature1 * 100) -  25  + (100 * 16 - remain * 100) / (16);
    retval = ((float)ds1820_temperature1/100);
    return retval;
}

void ds1820_search_applet(void)
{
    lcd_clear(Black);
    
    lcd_PutString(2, 3, "Get ROM code from\0 ", Blue, Black);
    lcd_PutString(2, 19, "Current DS1820\0 ", Blue, Black);
    lcd_PutString(2, 35, "(One at a time!)\0", Blue, Black);
    lcd_PutString(2, 53, "Display the temp\0", Blue, Black);
    lcd_PutString(2, 69, "of current sensor\0", Blue, Black);
    char * t1 = "HLT\0";
    char * t2 = "MASH\0";
    char * t3 = "CABINET\0";
    char * t4 = "AMBIENT\0";
    lcd_DrawRect(160, 0, 230, 100, Red);
    lcd_PutString(179, 46, "BACK\0", Red, Black); 
    lcd_draw_applet_options(t1, t2, t3, t4);
    
}

void ds1820_search_key(uint16_t x, uint16_t y){
    uint16_t window = 255;
    char code[30];   
    char lcd_string[30];
    char console[100];
    float sensor_temp = 0.00;
    static uint16_t last_window = 0; 
    
    if (touchIsInWindow(x,y, 0,0, 150,50) == pdTRUE)
        window = 0;
    
    else  if (touchIsInWindow(x,y, 0,50, 150,100) == pdTRUE)
        window = 1;
    
    
    else if (touchIsInWindow(x,y, 0,140, 120,220) == pdTRUE)
        window = 2;
    
    else  if (touchIsInWindow(x,y, 120,140, 239,220) == pdTRUE)
        window = 3;
    
    
    else  if (touchIsInWindow(x,y, 0,220, 120,300) == pdTRUE)
        window = 4;
    
    
    else  if (touchIsInWindow(x,y, 120,220, 239,300) == pdTRUE)
        window = 5;
    
    
    else  if (touchIsInWindow(x,y, 160, 0, 230,100) == pdTRUE)
        window = 6;
    
    //Back Button
    if (window == 6)
    {
     
        
    }
 
    else if (window == 0){
        
        ds1820_search();
        if (rom[0] == 0x10)    
            sprintf(code, "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\0",rom[0], 
                    rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7]);
        else sprintf(code, "NO SENSOR\0"); 
        lcd_PutString(2, 101, code, Blue, Black);
        
  
    }
    
    else if (window == 1){
        sensor_temp = ds1820_read_device(rom);
        sprintf(code, "Current Sensor = %.2f\0", sensor_temp);
        lcd_PutString(2, 117, code, Blue, Black);
        
        
    }
    else if (window == 2){
        
        if (rom[0] == 0x10)  {
           
            memcpy(b[HLT], rom, sizeof(rom)+1);
            sprintf(lcd_string, "COPIED\0"); 
            sprintf(console, "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\r\n Assigned to HLT\r\n",rom[0], 
                    rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7]);
            xQueueSendToBack(xConsoleQueue, &console, 0);
        }
        else sprintf(lcd_string, "NO SENSOR TO COPY\0"); 
        lcd_PutString(2, 160, lcd_string, Blue, Black);
        
    }
    else if (window == 3){
       
        if (rom[0] == 0x10)  {  
            memcpy(b[MASH], rom, sizeof(rom)+1);
            
            sprintf(lcd_string, "COPIED\0"); 
            sprintf(console, "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\r\n Assigned to MASH\r\n",rom[0], 
                    rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7]);
            xQueueSendToBack(xConsoleQueue, &console, 0);
        }
        else sprintf(lcd_string, "NO SENSOR TO COPY\0"); 
        lcd_PutString(121, 160, lcd_string, Blue, Black);
        
    }
    else if (window == 4){
        if (rom[0] == 0x10)  {  
             memcpy(b[CABINET], rom, sizeof(rom)+1);
             sprintf(lcd_string, "COPIED\0"); 
             sprintf(console, "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\r\n Assigned to CABINET\r\n",rom[0], 
                     rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7]);
             xQueueSendToBack(xConsoleQueue, &console, 0);
        }
        else sprintf(lcd_string, "NO SENSOR TO COPY\0"); 
        lcd_PutString(2, 250, lcd_string, Blue, Black);
    }
    
    else if (window == 5){
        if (rom[0] == 0x10)  {
            memcpy(b[AMBIENT], rom, sizeof(rom)+1);  
            sprintf(lcd_string, "COPIED\0"); 
            sprintf(console, "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\r\n Assigned to AMBIENT\r\n",rom[0], 
                    rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7]);
            xQueueSendToBack(xConsoleQueue, &console, 0);
        }
        else sprintf(code, "NO SENSOR TO COPY\0"); 
        lcd_PutString(121, 250, lcd_string, Blue, Black);
        
    }
    
}
void  ds1820_display_temps(void){

    char lcd_string[20];
    
    lcd_clear(Black);
    lcd_draw_back_button();
    lcd_PutString(1,1, "TEMPERATURES", Blue, Black);
    sprintf(lcd_string, "HLT = %.2f\0", ds1820_get_temp(HLT));
    lcd_PutString(1,40, lcd_string, Green, Black);
    sprintf(lcd_string, "Mash = %.2f\0", ds1820_get_temp(MASH));
    lcd_PutString(1,56, lcd_string, Green, Black);
    sprintf(lcd_string, "Cabinet = %.2f\0", ds1820_get_temp(CABINET));
    lcd_PutString(1,72, lcd_string, Green, Black);
    sprintf(lcd_string, "Ambient = %.2f\0", ds1820_get_temp(AMBIENT));
    lcd_PutString(1,88, lcd_string, Green, Black);
}

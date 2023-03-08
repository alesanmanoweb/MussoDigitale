#include <Adafruit_ADS1X15.h>   // adc
#include <Adafruit_GFX.h>       // graphics
#include <Adafruit_SSD1306.h>   // oled screen
#include <Preferences.h>        // to save the setpoint
#include <Wire.h>               // screen and adc
#include <ESP32Encoder.h>       // encoder
#include <Button2.h>            // start and encoder button
#include "logo_2_inv.h"         // splash screen logo

// screen 
#define SCREEN_WIDTH 128    // OLED display width, in pixels
#define SCREEN_HEIGHT 64    // OLED display height, in pixels
#define SCREEN_ADDRESS 0x3C
#define OLED_RESET -1       // Reset pin # (or -1 if sharing Arduino reset pin)
#define I2C_CLOCK 400000UL
#define I2C_CLOCK_AFTER 10000UL
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// encoder
#define ENCODER_A_PIN 32
#define ENCODER_B_PIN 33
#define CLICKS_PER_STEP 4
ESP32Encoder encoder;

// encoder button
#define ENCODER_BTN_PIN 25
Button2 encoderBtn;

// start button
#define START_BTN_PIN 26
Button2 startBtn;

// relay
#define RELAY_1_PIN 13
#define RELAY_2_PIN 16

// buzzer
#define BUZZER_PIN 4

// adc
Adafruit_ADS1115 ads;

// store setpoint in flash
Preferences preferences;

// global constants
const char motorAnimation[4] = {0x2F, 0x2D, 0x5C, 0x7C}; // motor animation characters
const int minSetPoint = 0;      // minimum user setpoint
const int maxSetPoint = 9999;   // maximum user setpoint
const int logSize = 120;        // currentReading log size
const int beepPeriod = 25;      // period to play beep, 25 = 1 second
const int wdtTimeout = 1000;    //time in ms to trigger the watchdog
const unsigned int currentLogPeriodMS = 1000; // update log 1 time a second
const unsigned int animationPeriodMS = 500;   // update motor animation 2 times a second
const unsigned int refreshPeriodMS = 40;      // update screen 25 times a second

// global variables
unsigned int currentReading = 0;  // current reading from adc
int setPoint = 0;           // user setpoint to turn off motor
int triggerCount = 0;       // number of consecutive values over setpoint
int editMode = 0;           // 0=off, 1=hundreds(upper 2 digits), 2=ones(lower 2 digits)
int motorAnimationIndex = 0;    // selected motor animation character
int currentLog[logSize];      // log to store current readings
int currentLogIndex = 0;      // points to the next index to write to
int playBeep = 0;         // sound buzzer while greater than 0
unsigned int logTicksMS = 0;    // time elapsed counter for log
unsigned int animationTicksMS = 0;  // time elapsed counter for motor animation
int oldEncoderCount = 0;      // previous encoder value
bool motorOn = false;       // commanded state of the motor relay
unsigned long startMS = 0;      // starting milliseconds, used for periodic tasks
unsigned long currentMS = 0;    // current milliseconds, used for periodic tasks
unsigned long diffMS = 0;     // millisecond period between loops, used for periodic tasks
hw_timer_t *wdTimer = NULL;     // watchdog timer

void setup()
{
  // setup screen
  // SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
    for(;;); // Don't proceed, loop forever
    
  // setup adc
  //ads.setGain(GAIN_ONE); // 1x gain   +/- 4.096V  1 bit = 0.125mV
  ads.setGain(GAIN_TWO); // 2x gain   +/- 2.048V  1 bit = 0.0625mV
  ads.begin();
    
  // setup relays
  pinMode(RELAY_1_PIN, OUTPUT);
  pinMode(RELAY_2_PIN, OUTPUT);
  digitalWrite(RELAY_1_PIN, LOW);
  digitalWrite(RELAY_2_PIN, LOW);

  // setup buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // setup start button, default pullup
  startBtn.begin(START_BTN_PIN);
  startBtn.setPressedHandler(startPressed);
  
  // setup encoder button, default pullup
  encoderBtn.begin(ENCODER_BTN_PIN);
  encoderBtn.setPressedHandler(encoderPressed);

  // setup encoder
  ESP32Encoder::useInternalWeakPullResistors=UP;
  encoder.attachSingleEdge(ENCODER_A_PIN, ENCODER_B_PIN);
  encoder.setCount(0);
  
  // get setpoint from flash
  preferences.begin("musso", false);
  setPoint = preferences.getInt("setpoint", 4000);
  
  // display the logo
  display.clearDisplay();
  display.drawBitmap(0, 0, image_data_logo, 128, 64, 1);
  display.display();

  delay(1500); 
  startMS = millis();
}

void loop()
{
  // update buttons
  encoderBtn.loop();
  startBtn.loop();

  // update setpoint
  if (editMode != 0)
    encoderUpdate();
    
  // update relay
  if(motorOn)
    digitalWrite(RELAY_1_PIN, HIGH);
  else
    digitalWrite(RELAY_1_PIN, LOW);
  
  
  // calculate refresh period
  currentMS = millis();
  diffMS = currentMS - startMS;
  if (diffMS >= refreshPeriodMS)
  { 
    // update times for refresh periods
    startMS = currentMS; 
    logTicksMS += diffMS;
    animationTicksMS += diffMS;
    
    // update current from adc
    getCurrentReading();

    // update display
    display.clearDisplay();
    printCurrent();
    printGraph();
    display.display();

    // update buzzer
    if(playBeep > 0){      
      digitalWrite(BUZZER_PIN, HIGH);
      playBeep--;
      if(playBeep == 0)        
        digitalWrite(BUZZER_PIN, LOW);     
    }

    // update animation
    if (animationTicksMS >= animationPeriodMS){
      motorAnimationIndex++;
      if(motorAnimationIndex >= 4)
        motorAnimationIndex = 0;
      animationTicksMS = 0;
    }

    // update log
    if(logTicksMS >= currentLogPeriodMS)
    {      
      currentLog[currentLogIndex] = currentReading;
      currentLogIndex++;
      if(currentLogIndex >= logSize)
        currentLogIndex = 0;

      checkSetpoint();  
      logTicksMS = 0;
    }        
  }
    
}

// print the current(top row) to the screen
void printCurrent()
{
  // get the individual digits
  int thousands = setPoint / 1000;
  int hundreds = (setPoint / 100) % 10;
  int tens = (setPoint / 10) % 10;  
  int ones = setPoint % 10;
  
  // setup the cursor for the current reading
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(0,1);

  // add spaces to the beginning to keep currentReading aligned on the top row
  if (currentReading<1000) display.print(' ');
  if (currentReading<100) display.print(' ');
  if (currentReading<10) display.print(' ');
  display.print(currentReading);
  display.print(' ');


  if(editMode == 1) // editing upper 2 digits
  {
    // draw colored background
    display.fillRect(59, 0, 24, 16, WHITE);   
    display.setTextColor(BLACK);
    
    // print the digits, replacing 0 with space
    if (setPoint<1000)
      display.print(' ');
    else    
      display.print(thousands);
    if (setPoint<100)
      display.print(' ');
    else
      display.print(hundreds);
    display.setTextColor(WHITE); // change the text color back
    if (setPoint<10)
      display.print(' ');
    else
      display.print(tens);
    display.print(ones);
  }
  else if(editMode == 2)  // editing lower 2 digits
  {
    // draw colored background
    display.fillRect(83, 0, 24, 16, WHITE);
    
    // print the digits, replacing 0 with space
    if (setPoint<1000)
      display.print(' ');
    else    
      display.print(thousands);
    if (setPoint<100)
      display.print(' ');
    else
      display.print(hundreds);
    display.setTextColor(BLACK);
    if (setPoint<10)
      display.print(' ');
    else
      display.print(tens);
    display.print(ones);
  }
  else // not editing, just print the setpoint
  {
    if (setPoint<1000) display.print(' ');
    if (setPoint<100) display.print(' ');
    if (setPoint<10) display.print(' ');
    display.print(setPoint);
  }
  
  if(motorOn) // draw spinning motor animation
    display.drawChar(115, 1, motorAnimation[motorAnimationIndex], WHITE, BLACK, 2);
}

// draw the current reading log graph on the lower part of the screen
void printGraph()
{
  int index = 0, val = 0, graphMax = maxSetPoint;
  
  // draw the graph border
  display.drawFastHLine(0, 16, 128, WHITE);
  display.drawFastHLine(120, 28, 4, WHITE);
  display.drawFastHLine(120, 40, 7, WHITE);
  display.drawFastHLine(120, 52, 4, WHITE);
  display.drawFastHLine(120, 63, 7, WHITE);
  display.drawFastVLine(120, 16, 48, WHITE);

  // graph upper boundary is equal to the setpoint 
  if(setPoint != 0)
    graphMax = setPoint;

  // iterate over the entire log
  for(int i=0; i<logSize; i++)
  {
    // start at the oldest log value
    index = currentLogIndex + i;
    if (index >= logSize) // wrap around to zero
      index -= logSize;

    // scale the log value to a pixel X value
    val = 63 - map(currentLog[index], minSetPoint, graphMax, 0, 48);
    
    if(val <= 16)
    {
      // if pixel is at or above the max graph, display it in inverted color
      val = 16;
      display.drawPixel(i, val, BLACK);
    }
    else
      display.drawPixel(i, val, WHITE);
  }
}

// start button handler
void startPressed(Button2& btn)
{
  motorOn = !motorOn;
  triggerCount = 0;
}

// encoder button handler
void encoderPressed(Button2& btn)
{
  editMode++;     // cycle edit mode
  oldEncoderCount = 0;// reset encoder count
  encoder.setCount(0);// reset encoder count
  
  if (editMode > 2) // save setpoint to memory
  {
    editMode = 0;
    preferences.putInt("setpoint", setPoint);
  }
}

// encoder rotation handler
void encoderUpdate()
{
  int dir = 0, newSetPoint = 0;
  int encCnt = encoder.getCount();
  
  // determine encoder direction
  if (oldEncoderCount > encCnt)
    dir = -1;
  else if(oldEncoderCount < encCnt)
    dir = 1;
  oldEncoderCount = encCnt;

  // update the setpoint
  if(editMode == 1)
    newSetPoint = setPoint + (100 * dir);
  else if(editMode == 2)
    newSetPoint = setPoint + (1 * dir);
  
  // clip setpoint at min and max
  if(newSetPoint > maxSetPoint)
    newSetPoint = maxSetPoint;
  else if (newSetPoint < minSetPoint)
    newSetPoint = minSetPoint;

  setPoint = newSetPoint;
}

// check if setpoint is exceeded
void checkSetpoint()
{
  // if setpoint is 0 run forever
  if(setPoint != 0 && currentReading >= setPoint)
    triggerCount++; 
  else
    triggerCount = 0;

  // if setpoint is exceeded 3 times in a row, stop motor
  if(triggerCount >= 3)
  {
    triggerCount = 0;
    if(motorOn)
    {
      motorOn = false;
      playBeep = beepPeriod;
    }
  }  
}

// get the current reading from adc
void getCurrentReading()
{
  // TODO: maybe add some filtering here
  int16_t adc0 = ads.readADC_SingleEnded(0);
  if (adc0 > 9999)
    adc0 = 9999;
 
  currentReading = adc0;
}

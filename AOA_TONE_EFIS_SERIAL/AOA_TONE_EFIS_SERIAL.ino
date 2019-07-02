////////////////////////////////////////////////////
////////////////////////////////////////////////////
// AOA EFIS Serial to audio tone 
// Supports: Dynon EFIS data using a Arduino Due board
// Ver 1.6.1
// Christopher Jones 6/10/2016
//
// - Read data in from Serial3 on DUE board. (Dynon is at 115200 baud)
// - Serial is used for debugging to computer IDE. (115200 baud)
// - Audio ouput on pin 2
// - If no serial data is detected audio tone turns off and LED1 is off
// - MUTE_AUDIO_UNDER_IAS define is used to only activate audio tones if above a given airspeed
//
// 6/10/2016  - Started
// 7/09/2018  - 1.5.1 fix for tone PPS between solid tones.  was not calucating the right ratio for the PPS changes.
// 9/02/2018  - 1.5.2 Atempts to fix hang up issues with skyview data.  Changed start up tones.  added more comments. fix min PPS for low tone.
// 9/28/2018  - 1.6.0 Fixed Skyview lockup issue.
// 9/28/2018  - 1.6.1 allow for automatic efis dectection d100 or skyview.

#include <DueTimer.h>         // timer lib functions for using DUE timers and callbacks.
#include <stdint.h>
#include <Arduino.h>

#include <Gaussian.h>         // gaussian lib used for avg out AOA values.
//#include <LinkedList.h>       // linked list is also required.
#include <GaussianAverage.h>  // more info at https://github.com/ivanseidel/Gaussian

// Output serial debug infomation. (comment out the following line to turn off serial debug output)
#define SHOW_SERIAL_DEBUG 

// Average out AOA values to give a smoother transtition between values.
//#define USE_AOA_AVERAGE
// set how many previous AOA values we store to create avg and help smooth out the data values.
#define AOA_HISTORY_MAX       5

// set Freq of callback function used to create the pulse tones.
#define FREQ_OF_FUNCTION      100

// set the sample rate of how often to pull the AOA value from the serial stream.
// example 
// 1 = sample every AOA value received from the efis.
// 5 = skip every 5 lines then sample the AOA value.
#define SERIAL_SAMPLE_RATE    3

// Min airspeed in knots before the audio tone is turned on
// This is useful because if your on the ground you don't need a beeping in your ear.
#define MUTE_AUDIO_UNDER_IAS  5

// Expected serial string lengths
#define DYNON_SERIAL_LEN              53
#define DYNON_SKYVIEW_SERIAL_LEN      74

// array postions for aoa values
#define POS_HIGH_TONE_AOA_STALL   0      // % (and above) where stall happens.
#define POS_HIGH_TONE_AOA_START   1      // % (and above) where high tone starts
#define POS_LOW_TONE_AOA_SOLID    2      // % (and above) where a solid low tone is played.
#define POS_LOW_TONE_AOA_START    3      // % (and above) where low 
#define D100      0
#define SKYVIEW   1
int AoaToneMatrix[2][5];          // tone matrix for different efis units.
unsigned char WhichEFIS = D100;   // detect which efis is being read and set store it here.

// general tone settings. Tone Pulse Per Sec (PPS)  (used for all efis units)
#define HIGH_TONE_STALL_PPS       20      // how many PPS to play during stall
#define HIGH_TONE_PPS_MAX         6.5     // 6.5   
#define HIGH_TONE_PPS_MIN         1.5     // 1.5
#define HIGH_TONE_HZ              1600    // freq of high tone
//#define HIGH_TONE2_HZ             1500    // a 2nd high tone that it will cycle between (if defined)
#define LOW_TONE_PPS_MAX          8.5
#define LOW_TONE_PPS_MIN          1.5
#define LOW_TONE_HZ               400     // freq of low tone

#define BAUDRATE_EFIS         115200  // baud rate for D100/D10 and dynon
#define TONE_PIN              2     // TIOA0
#define PIN_LED1              13    // internal LED for showing AOA status.
#define PIN_LED2              54    // aka A0. external LED for showing serial input.
#define PULSE_TONE            1
#define SOLID_TONE            2
#define TONE_OFF              3
#define STARTUP_TONES_DELAY   120
#define STARTUP_TONES_DELAY2  300

uint8_t toneState = false;
unsigned char toneMode = PULSE_TONE;  // current mode of tone.  PULSE_TONE, SOLID_TONE, or TONE_OFF
boolean highTone = false;             // are we playing high tone or low tone?
uint32_t toneFreq = 0;                // store current freq of tone playing.
float pps = 0;                        // store current PPS of tone (used for debuging) 
int liveAOA;                          // realtime AOA value.
int AOA = 0;                          // avaraged AOA value is stored here.
int lastAOA = 0;                      // save last AOA value here.
unsigned int ALT = 0;                 // hold ALT (only used for debuging)
int ASI = 0;                          // live Air Speed Indicated
unsigned int cyclesWOSerialData = 0;  // keep track if not serial data is recieved.
//unsigned char efisTypeDetected = 0;   // auto detect which efis is being used. 0 no detection. 1 = D series, 2 = Skyview
GaussianAverage myAverageAOA = GaussianAverage(AOA_HISTORY_MAX);
static Tc *chTC = TC0;
static uint32_t chNo = 0;

// vars for converting AOA scale value to PPS scale value.
int OldRange,  OldValue;
float NewRange, NewValue;

char inChar;                    // store single serial input char here.
#define MAXSIZE 90              // max length of string
char input[MAXSIZE+1];          // buffer for full serial data input string.
int inputPos = 0;               // current postion of serial data.
char tempBuf[90];               // misc char buffer used for debug

void setup() {
  // put your setup code here, to run once:
  pinMode(TONE_PIN, OUTPUT);
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);

  Serial.begin(115200);   //Init hardware serial port (ouput to computer for debug)
  Serial3.begin(BAUDRATE_EFIS);  //Init hardware serial port (input from EFIS)

  configureToneTimer();   //setup timer used for tone

  // startup tone sequence.
  digitalWrite(PIN_LED2, 1);
  setFrequencytone(400);
  delay(STARTUP_TONES_DELAY2);
  setFrequencytone(600);
  delay(STARTUP_TONES_DELAY2);
  setFrequencytone(800);
  delay(STARTUP_TONES_DELAY2);
  setFrequencytone(0);
  digitalWrite(PIN_LED2, 0);
  
  // timer callback is used for turning tone on/off.
  Timer4.attachInterrupt(tonePlayHandler);
  Timer4.setFrequency(FREQ_OF_FUNCTION);  // set how often we should run the callback function.
  Timer4.start();

  // Dynon Skyview settings.
  // For skyview export ahrs data out one of the 5 serialports (only needs serial output to AOA box, no serial input needed)
  AoaToneMatrix[SKYVIEW][POS_HIGH_TONE_AOA_STALL] = 80;      // % (and above) where stall happens.
  AoaToneMatrix[SKYVIEW][POS_HIGH_TONE_AOA_START] = 66;      // % (and above) where high tone starts
  AoaToneMatrix[SKYVIEW][POS_LOW_TONE_AOA_SOLID]  = 55;     // % (and above) where a solid low tone is played.
  AoaToneMatrix[SKYVIEW][POS_LOW_TONE_AOA_START]  = 40;      // % (and above) where low 
  
  // Dynon D100 and D10 settings.
  AoaToneMatrix[D100][POS_HIGH_TONE_AOA_STALL] =  72;      // % (and above) where stall happens.
  AoaToneMatrix[D100][POS_HIGH_TONE_AOA_START] =  32;      // % (and above) where high tone starts
  AoaToneMatrix[D100][POS_LOW_TONE_AOA_SOLID]  =  22;      // % (and above) where a solid low tone is played.
  AoaToneMatrix[D100][POS_LOW_TONE_AOA_START]  =   0;      // % (and above) where low 

}

// We use our own counter for how often we should pause between tones (pulses per sec PPS)
int cycleCounter = 0;
int cycleCounterResetAt = FREQ_OF_FUNCTION;
uint8_t Tone2FlipFlop = false;
void tonePlayHandler(){
  cycleCounter++;
  // check if our counter has reach the reset mark.  if so flip the tone on/off and start again.
  // cycleCounterResetAt is set by the PPS set function.
  if(cycleCounter >= cycleCounterResetAt) {
    toneState = toneState ^ 1;  // flip tone state
    cycleCounter = 0;
  } else {
    return;
  }
  
  if(toneMode==TONE_OFF) {
    setFrequencytone(0);  // if tone off skip the rest.
    return;
  }
  if(toneMode==SOLID_TONE) {  // check for a solid tone.
#ifdef SHOW_SERIAL_DEBUG    
    Serial.println("SOLID TONE");
#endif    
    setFrequencytone(LOW_TONE_HZ);
    return; // skip the rest
  }

  // cylce tone on/off depending on toneState which is flipped in code above.
  if(toneState) {
     digitalWrite(PIN_LED1, digitalRead(PIN_LED1)^1);  // cycle led on/off
     //sprintf(tempBuf, "handler() AOA:%i ASI:%i tone: %i PPS:%f handlerFreq: %i",AOA,ASI,toneFreq,pps, handlerFreq);
     //Serial.println(tempBuf);
     if(highTone) {
// check if we want 2 different tones for the high tone mode.      
#ifdef HIGH_TONE2_HZ
        Tone2FlipFlop = Tone2FlipFlop ^ 1; 
        if(Tone2FlipFlop)
          setFrequencytone(HIGH_TONE_HZ);
        else
          setFrequencytone(HIGH_TONE2_HZ);
#else
        setFrequencytone(HIGH_TONE_HZ);
#endif
     } else {
        setFrequencytone(LOW_TONE_HZ);
     }

  } else {
    setFrequencytone(0);
  }
}

void checkAOA() {
  if(ASI <= MUTE_AUDIO_UNDER_IAS) {
#ifdef SHOW_SERIAL_DEBUG    
  // show audio muted and debug info.
  sprintf(tempBuf, "AUDIO MUTED: Airspeed to low. Min:%i ASI:%i",MUTE_AUDIO_UNDER_IAS, ASI);
  Serial.println(tempBuf);
#endif
  toneMode = TONE_OFF;
  return;
  }
  
  if(lastAOA == AOA) {
    return; // skip this if the AOA value has not changed.
  }
  
  // check AOA value and set tone and pauses between tones according to 
  if(AOA >= AoaToneMatrix[WhichEFIS][POS_HIGH_TONE_AOA_STALL]) {
    // play 20 pps HIGH tone
    highTone = true;
    setPPSTone(HIGH_TONE_STALL_PPS);
    toneMode = PULSE_TONE;
  } else if(AOA >= AoaToneMatrix[WhichEFIS][POS_HIGH_TONE_AOA_START]) {
    // play HIGH tone at Pulse Rate 1.5 PPS to 6.2 PPS (depending on AOA value)
    highTone = true;
    OldValue = AOA-(AoaToneMatrix[WhichEFIS][POS_HIGH_TONE_AOA_START]-1);
    toneMode = PULSE_TONE;
    // scale number using this. http://stackoverflow.com/questions/929103/convert-a-number-range-to-another-range-maintaining-ratio
    OldRange = AoaToneMatrix[WhichEFIS][POS_HIGH_TONE_AOA_STALL] - AoaToneMatrix[WhichEFIS][POS_HIGH_TONE_AOA_START]; //20 - 1;  //(OldMax - OldMin)  
    NewRange = HIGH_TONE_PPS_MAX - HIGH_TONE_PPS_MIN; // (NewMax - NewMin)  
    NewValue = (((OldValue - 1) * NewRange) / OldRange) + HIGH_TONE_PPS_MIN; //(((OldValue - OldMin) * NewRange) / OldRange) + NewMin
    setPPSTone(NewValue);
  } else if(AOA >= AoaToneMatrix[WhichEFIS][POS_LOW_TONE_AOA_SOLID]) {
    // play a steady LOW tone
    highTone = false;
    toneMode = SOLID_TONE;
  } else if(AOA > AoaToneMatrix[WhichEFIS][POS_LOW_TONE_AOA_START]) {
    toneMode = PULSE_TONE;
    highTone = false;
    // play LOW tone at Pulse Rate 1.5 PPS to 8.2 PPS (depending on AOA value)
    // scale number using this. http://stackoverflow.com/questions/929103/convert-a-number-range-to-another-range-maintaining-ratio
    OldValue = AOA - AoaToneMatrix[WhichEFIS][POS_LOW_TONE_AOA_START];
    OldRange = AoaToneMatrix[WhichEFIS][POS_LOW_TONE_AOA_SOLID] - AoaToneMatrix[WhichEFIS][POS_LOW_TONE_AOA_START]; //40 - 1;  //(OldMax - OldMin)  
    NewRange = LOW_TONE_PPS_MAX - LOW_TONE_PPS_MIN; // (NewMax - NewMin)  
    NewValue = (((OldValue - 1) * NewRange) / OldRange) + LOW_TONE_PPS_MIN; //(((OldValue - OldMin) * NewRange) / OldRange) + NewMin
    setPPSTone(NewValue);
  } else {
    toneMode = TONE_OFF;
  }

  lastAOA = AOA;
#ifdef SHOW_SERIAL_DEBUG    
  // show serial debug info.
  sprintf(tempBuf, "TONE AOA:%i Live:%i ASI:%ikts ALT:%i PPS:%f cycleCounterResetAt: %i",AOA, liveAOA, ASI, ALT, pps, cycleCounterResetAt);
  Serial.println(tempBuf);
#endif
}

void setPPSTone(float newPPS) {
  // set PPS by setting cycleCounterResetAt which is used in the tonePlayHandler()
  cycleCounterResetAt = (1/(newPPS*1.5)) * FREQ_OF_FUNCTION;
  pps = newPPS;  // store pps for debug purposes.
}

// counter for how often to cycle led and sample AOA from serial data.
int LedCountDown = SERIAL_SAMPLE_RATE; 
// main loop of app
void loop() {
      // check for serial in from efis.
      while(Serial3.available()) {
        inChar = (char)Serial3.read();  // read in char
        input[inputPos]=inChar;         // save into string buffer.
        inputPos++;                     // increment the string buffer postion.
        cyclesWOSerialData = 0;         // reset the var thats keep track of when we last had serial data (used for turning off tone if no serial data found)

        // check for dynon skyview data. based on length of string and if the 1st 2 chars match
        if (inChar == '\n' && inputPos == DYNON_SKYVIEW_SERIAL_LEN && input[0]=='!' && input[1]=='1') {
          WhichEFIS = SKYVIEW;  // set skyview efis detected.
          // skyview data starts with a '!1'... the D series efis has no line prefix.  
          tempBuf[0] = input[43]; // get the 2 bytes for aoa on skyview.
          tempBuf[1] = input[44]; // 
          tempBuf[2] = '\0'; // term string  (needed for when we convert a string to a int in the next line)
          liveAOA = strtol(tempBuf, NULL, 10); //convert to int

          // get AOA value (check if we are using a Gaussian function to smooth the values)
#ifdef USE_AOA_AVERAGE
          myAverageAOA += liveAOA;            // store and calcute the mean average AOA from previous values.
          AOA = myAverageAOA.process().mean;
#else          
          AOA = liveAOA;  // else just use the live AOA value.
#endif
          // get ASI (4 digits) but we only want the first 3. we don't need a 10/th of a knot
          tempBuf[0] = input[23]; //
          tempBuf[1] = input[24]; //
          tempBuf[2] = input[25]; //
          tempBuf[3] = '\0'; //
          ASI = strtol(tempBuf, NULL, 10); //convert to int (data comes in knots already on skyview)

#ifdef SHOW_SERIAL_DEBUG    
  // show serial debug info for skyview data found.
  // can't edit the tempBuf here.. was causing crash. Christopher. 9/26/2018
  //sprintf(tempBuf, "SKYVIEW AOA:%i Live:%i ASI:%ikts ALT:%i PPS:%f cycleCounterResetAt: %i",AOA, liveAOA, ASI, ALT, pps, cycleCounterResetAt);
  //Serial.println(tempBuf);
   Serial.print("SKYVIEW AOA ");
   Serial.println(AOA);
#endif
          validAOADataFound();  // run function to process tone.  because we found a valid AOA value.
          
        } else if (inChar == '\n' && inputPos == DYNON_SERIAL_LEN) {  // is EOL?
          WhichEFIS = D100;  // set D100 efis detected
          // else check for dynon d series data.
          // efisTypeDetected != SKYVIEW has been added to make sure we don't confused skyview data for d10 data and try to process..
          // get AOA from dynon efis string.  details of format can be found from dynon pdf manual.
          tempBuf[0] = input[39]; //
          tempBuf[1] = input[40]; //
          tempBuf[2] = '\0'; //term string (needed for converting a string to a int in the next line)
          liveAOA = strtol(tempBuf, NULL, 10); //convert to int

          // get AOA value (check if we are using a Gaussian function to smooth the values)
#ifdef USE_AOA_AVERAGE
          myAverageAOA += liveAOA;            // store and calcute the mean average AOA from previous values.
          AOA = myAverageAOA.process().mean;
#else          
          AOA = liveAOA;  // else just use the live AOA value.
#endif
      
          // get ASI (4 digits) but we only want the first 3 because its in meters per second.
          tempBuf[0] = input[20]; //
          tempBuf[1] = input[21]; //
          tempBuf[2] = input[22]; //
          tempBuf[3] = '\0'; // terminate string (needed for converting a string to a int in the next line)
          ASI = strtol(tempBuf, NULL, 10) * 1.943; //convert to int (and from m/s to knots)

          // get ALT (4 digits) in meters.. this is really not used for this application.  we just collect it to show in debug.
          tempBuf[0] = input[25]; //
          tempBuf[1] = input[26]; //
          tempBuf[2] = input[27]; //
          tempBuf[3] = input[28]; //
          tempBuf[4] = '\0'; //
          ALT = (strtol(tempBuf, NULL, 10) * 0.328) * 10; //convert to long (and from meters to feet)
          validAOADataFound();

#ifdef SHOW_SERIAL_DEBUG    
          Serial.print("D100 AOA ");
          Serial.println(AOA);
#endif
        } else if(inChar == '\n') {
          // if we are at the end of line then reset postion to start looking for a new line.
          inputPos = 0; 
        } else if(inputPos>DYNON_SKYVIEW_SERIAL_LEN) {
          // else safety check, if we went past the max number of chars then reset to zero
          inputPos = 0;
#ifdef SHOW_SERIAL_DEBUG    
        Serial.println("OVER FLOW DATA");
#endif
        }
        
      }

      // check if no serial has been sent in a while.
      cyclesWOSerialData++;
      if(cyclesWOSerialData > 55500) {
        //TODO:  should we do something more to inidicat to the user there is no serial data coming in?  this might be useful for troubleshooting after a install.
        toneMode = TONE_OFF;        // stop tone
        cyclesWOSerialData == 0;
        digitalWrite(PIN_LED2, 0);  // no EFIS serial data so turn off led.
#ifdef SHOW_SERIAL_DEBUG    
        Serial.println("NO DATA");
#endif
      } 
}

// valid AOA data was found so this function will be ran.  Here we toggle the leds and see how often we should sample the AOA data.
void validAOADataFound() {
  // check how often we want to sample the serial data.
  LedCountDown --;
  if(LedCountDown<1) {
     digitalWrite(PIN_LED2, digitalRead(PIN_LED2)^1);  // cycle serial RX led on/off
     LedCountDown = SERIAL_SAMPLE_RATE;
     checkAOA();  // check AOA data from dynon and store result into AOA var.
  }
  inputPos = 0;  // reset postion to start of buffer string for next round.
}

void configureToneTimer() {
  // Configure TONE_PIN pin as timer output
  // pmc_enable_periph_clk( ID_PIOB ) ;
  int result = PIO_Configure( PIOB,
            PIO_PERIPH_B,
            PIO_PB25B_TIOA0,
            PIO_DEFAULT);

  pmc_set_writeprotect(false);
  pmc_enable_periph_clk(ID_TC0);
 TC_Configure(chTC, chNo,
         TC_CMR_TCCLKS_TIMER_CLOCK4 |
         TC_CMR_WAVE |         // Waveform mode
         TC_CMR_WAVSEL_UP_RC | // Counter running up and reset when equals to RC
         TC_CMR_ACPA_SET |     // RA compare sets TIOA
         TC_CMR_ACPC_CLEAR );  // RC compare clears TIOA
         
  chTC->TC_CHANNEL[chNo].TC_IER=TC_IER_CPCS;  // RC compare interrupt
  chTC->TC_CHANNEL[chNo].TC_IDR=~TC_IER_CPCS;
  //NVIC_EnableIRQ(TC0_IRQn);
}

const uint32_t volHigh = 50; 
const uint32_t volMed = 10; 
const uint32_t volLow = 1; 

void setFrequencytone(uint32_t frequency)
{
  setFrequencytoneAndVol(frequency,volHigh);
}

void setFrequencytoneAndVol(uint32_t frequency,uint32_t vol)
{
  
  if(frequency < 20 || frequency > 20000) {
    //Serial.print("cancel tone: ");Serial.println(frequency);
    TC_Stop(chTC, chNo);
    toneFreq = frequency;
    return;
  }

  if(toneFreq == frequency) {
    // if the new frequency is the same as the current freq then don't do anything.
    return;
  }
  
  const uint32_t rc = VARIANT_MCK / 128 / frequency; 
  //const uint32_t ra = 50; //rc >> 2; // 50% duty cycle 
  //const uint32_t rb = 50;//ra >> 2; // 20% duty cycle 

  //Serial.print("rc=");Serial.println(rc);
  //Serial.print("ra=");Serial.println(ra);
  //Serial.print("rb=");Serial.println(rb);

  TC_Stop(chTC, chNo);
  TC_SetRC(chTC, chNo, rc);    // set frequency
  TC_SetRA(chTC, chNo, vol);   // duty cycle 
  TC_SetRB(chTC, chNo, vol);    
  TC_Start(chTC, chNo);
  toneFreq = frequency;
  
}


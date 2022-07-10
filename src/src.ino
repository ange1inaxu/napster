#include <WiFi.h>
#include <mpu6050_esp32.h>
#include<math.h>
#include<string.h>
#include <TFT_eSPI.h> 
#include <SPI.h>

TFT_eSPI tft = TFT_eSPI();  

char network[] = "MIT";
char password[] = "";

uint8_t channel = 1; //network channel on 2.4 GHz
byte bssid[] = {0x04, 0x95, 0xE6, 0xAE, 0xDB, 0x41}; //6 byte MAC address of AP you're targeting.

const uint8_t LOOP_PERIOD = 10; //milliseconds
uint32_t primary_timer = 0;
uint32_t posting_timer = 0;
const char USER[] = "artiste";

const int RESPONSE_TIMEOUT = 6000; //ms to wait for response from host
const int POSTING_PERIOD = 6000; //periodicity of getting a number fact.
const uint16_t IN_BUFFER_SIZE = 1000; //size of buffer to hold HTTP request
const uint16_t OUT_BUFFER_SIZE = 1000; //size of buffer to hold HTTP response
char request_buffer[IN_BUFFER_SIZE]; //char array buffer to hold HTTP request
char old_response[OUT_BUFFER_SIZE]; //char array buffer to hold HTTP request
char response[OUT_BUFFER_SIZE]; //char array buffer to hold HTTP response
char post_request_buffer[IN_BUFFER_SIZE]; //char array buffer to hold HTTP request
char post_response[OUT_BUFFER_SIZE]; //char array buffer to hold HTTP response



// Minimalist C Structure for containing a musical "riff"
struct Riff {
  double notes[1024]; //the notes (array of doubles containing frequencies in Hz. I used https://pages.mtu.edu/~suits/notefreqs.html
  int length; //number of notes (essentially length of array.
  float note_period; //the timing of each note in milliseconds (take bpm, scale appropriately for note (sixteenth note would be 4 since four per quarter note) then
};


Riff song_to_play; // Riff for user to select via GET
Riff song_to_create;  // Riff for user to create and POST

char notes[8000];
char riff_format[8000] = "";

const uint32_t READING_PERIOD = 150; //milliseconds
double MULT = 1.059463094359; //12th root of 2 (precalculated) for note generation
double A_1 = 55; //A_1 55 Hz  for note generation
const uint8_t NOTE_COUNT = 97; //number of notes set at six octaves from

//buttons for today 
uint8_t BUTTON1 = 45;
uint8_t BUTTON2 = 39;

//pins for LCD and AUDIO CONTROL
uint8_t LCD_CONTROL = 21;
uint8_t AUDIO_TRANSDUCER = 14;

//PWM Channels. The LCD will still be controlled by channel 0, we'll use channel 1 for audio generation
uint8_t LCD_PWM = 0;
uint8_t AUDIO_PWM = 1;

//arrays you need to prepopulate for use in the run_instrument() function
double note_freqs[NOTE_COUNT];
float accel_thresholds[NOTE_COUNT + 1];

//global variables to help your code remember what the last note was to prevent double-playing a note which can cause audible clicking
float new_note = 0;
float old_note = 0;

float last_read = 0.0;
MPU6050 imu; //imu object called, appropriately, imu
float x;


uint8_t state;
const uint8_t PLAYBACK = 1;
const uint8_t RECORD = 2;


void setup() {
  Serial.begin(115200);
  while (!Serial); // wait for Serial to show up

  connect_wifi();
  
  Wire.begin();
  delay(50); //pause to make sure comms get set up
  if (imu.setupIMU(1)) {
    Serial.println("IMU Connected!");
  } else {
    Serial.println("IMU Not Connected :/");
    Serial.println("Restarting");
    ESP.restart(); // restart the ESP (proper way)
  }
  tft.init(); //initialize the screen
  tft.setRotation(2); //set rotation for our layout
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  Serial.printf("Frequencies:\n"); //print out your frequencies as you make them to help debugging
  double note_freq = A_1;
  //fill in note_freq with appropriate frequencies from 55 Hz to 55*(MULT)^{NOTE_COUNT-1} Hz
  for(int i=0; i < NOTE_COUNT; i++){
    Serial.println(note_freq);
    note_freqs[i] = note_freq;
    note_freq *= MULT;
  }

  //print out your accelerometer boundaries as you make them to help debugging
  Serial.printf("Accelerometer thresholds:\n");
  //fill in accel_thresholds with appropriate accelerations from -1 to +1
  float accel = -1.0;
  float interval = 2.0 / NOTE_COUNT;
  for(int i=0; i <= NOTE_COUNT; i++){
    Serial.println(accel);
    accel_thresholds[i] = accel;
    accel += interval;
  }

  //start new_note as at middle A or thereabouts.
  new_note = note_freqs[NOTE_COUNT - NOTE_COUNT / 2]; //set starting note to be middle of range.

  //four pins needed: two inputs, two outputs. Set them up appropriately:
  pinMode(BUTTON1, INPUT_PULLUP);
  pinMode(BUTTON2, INPUT_PULLUP);
  pinMode(AUDIO_TRANSDUCER, OUTPUT);
  pinMode(LCD_CONTROL, OUTPUT);

  //set up AUDIO_PWM which we will control in this lab for music:
  ledcSetup(AUDIO_PWM, 200, 12);//12 bits of PWM precision
  ledcWrite(AUDIO_PWM, 0); //0 is a 0% duty cycle for the NFET
  ledcAttachPin(AUDIO_TRANSDUCER, AUDIO_PWM);

  //set up the LCD PWM and set it to 
  pinMode(LCD_CONTROL, OUTPUT);
  ledcSetup(LCD_PWM, 100, 12);//12 bits of PWM precision
  ledcWrite(LCD_PWM, 0); //0 is a 0% duty cycle for the PFET...increase if you'd like to dim the LCD.
  ledcAttachPin(LCD_CONTROL, LCD_PWM);

  state = PLAYBACK;  
}


//used to get x,y values from IMU accelerometer!
void get_angle(float* x, float* y) {
  imu.readAccelData(imu.accelCount);
  *x = imu.accelCount[0] * imu.aRes;
  *y = imu.accelCount[1] * imu.aRes;
}

// used to query for a song based on song_id
void lookup(char* query, char* response) {
  char request_buffer[200];
  sprintf(request_buffer, "GET /esp32test/limewire?song_id=%s HTTP/1.1\r\n", query);
  strcat(request_buffer, "Host: iesc-s3.mit.edu\r\n");
  strcat(request_buffer, "\r\n"); //new line from header to body
  char host[] = "iesc-s3.mit.edu";
  do_http_GET(host, request_buffer, response, OUT_BUFFER_SIZE, RESPONSE_TIMEOUT, true);
}

//enum for button states
enum button_state {S0, S1, S2, S3, S4};

class Button{
  public:
  uint32_t S2_start_time;
  uint32_t button_change_time;    
  uint32_t debounce_duration;
  uint32_t long_press_duration;
  uint8_t pin;
  uint8_t flag;
  uint8_t button_pressed;
  button_state state; // This is public for the sake of convenience
  Button(int p) {
  flag = 0;  
    state = S0;
    pin = p;
    S2_start_time = millis(); //init
    button_change_time = millis(); //init
    debounce_duration = 10;
    long_press_duration = 1000;
    button_pressed = 0;
  }
  void read() {
    uint8_t button_val = digitalRead(pin);  
    button_pressed = !button_val; //invert button
  }
  int update() {
    read();
    flag = 0;
    if (state==S0) {
      if (button_pressed) {
        state = S1;
        button_change_time = millis();
      }
    } else if (state==S1) {
      if (button_pressed && millis()-button_change_time >= debounce_duration){
        state = S2;
        S2_start_time = millis();
      } else if (!button_pressed){
        state = S0;
        button_change_time = millis();
      }
    } else if (state==S2) {
      if (button_pressed && millis()-S2_start_time >= long_press_duration){
        state = S3;
      } else if (!button_pressed){
        state = S4;
        button_change_time = millis();
      }
    } else if (state==S3) {
      if (!button_pressed){
        state = S4;
        button_change_time = millis();
      }
    } else if (state==S4) {        
      if (!button_pressed && millis()-button_change_time >= debounce_duration){
        state = S0;
        if (millis()-S2_start_time < long_press_duration){
          flag = 1;
        } else if (millis()-S2_start_time >= long_press_duration){
          flag = 2;
        }
      } else if (button_pressed && millis()-S2_start_time < long_press_duration){
        state = S2;
        button_change_time = millis();
      } else if (button_pressed && millis()-S2_start_time >= long_press_duration){
        state = S3;
        button_change_time = millis();
      }
    }
    return flag;
  }
};


Button button1(BUTTON1); //button object!
Button button2(BUTTON2); //button object!


// Slice original[start:end] and store in result
void slice(char* original, char* result, int start, int end){
  int i = 0;
  for (int j=start; j<end; j++){
    result[i] = original[j];
    i++;
  }
}


// SongIDGetter class and FSM to GET a song based on song_id
class SongIDGetter {
    char digits[50] = "0123456789";
    char msg[400] = {0}; //contains previous query response
    char query_string[50] = {0};
    int char_index;
    int state;
    uint32_t scroll_timer;
    const int scroll_threshold = 200;
    const float angle_threshold = 0.3;
  public:

    SongIDGetter() {
      state = 0;
      memset(msg, 0, sizeof(msg));//empty it.
      strcat(msg, "Long Press to input a song ID to play!");
      char_index = 0;
      scroll_timer = millis();
    }
    void update(float angle, int button, char* output) {      
      
      if (state == 0){ // State 0 - Message Display
        strcpy(output, msg);
        if (button == 2){ // long button press state
          state = 1;
          strcpy(query_string, "");
          char_index = 0;
          scroll_timer = millis();
        }
      }
      
      else if (state == 1){ // State 1 - Text Entry
        
        // short button press: concat the letter to our query_string and display output
        if (button == 1){
          char letter[4] = "";
          letter[0] = digits[char_index];
          strcat(query_string, letter);
          strcpy(output, query_string);
          char_index=0;
        }
        // long button press: transition to new state
        else if (button == 2){
          state = 2;
          strcpy(output, "");
        }

        // otherwise, use the IMU angle threshold to determine the digit
        else if (millis() - scroll_timer >= scroll_threshold){
          scroll_timer = millis();
          // When the board is tilted to the right, increment the currently selected character by one ('1' -> '2', '2' -> '3', ..., '9' -> '0')
          if (angle >= angle_threshold){
            char_index = (char_index + 1) % 11;
          }
          // When the board is tiled to the left, decrement the currently selected character by one ('0' -> '9', ..., '3' -> '2', '2' -> '1')
          else if (angle <= -angle_threshold) {
            char_index = (char_index - 1) % 11;
            if (char_index < 0){
              char_index += 11;
            }
          }
        }

        if (button != 2){
          sprintf(output, "%s%c", query_string, digits[char_index]);
        }
      }
    
    else if (state == 2){ //State 2 - Send Query
      state = 3;
      strcat(output, "Sending Query");
    }
    
    else if (state == 3){ // State 3 - Send/Receive Result
      lookup(query_string, response);
      strcpy(query_string, "");

      Serial.println("\nResponse Buffer: ");
      Serial.println(response);
      
      // Parse response (msg)

      //get the note period
      char note_period[10] = "";
      for (int i=0; i<strlen(response); i++){
        if (response[i] == '&'){
          slice(response, notes, i+1, strlen(response));
          Serial.println("Notes");
          Serial.println(notes);
          break;
        }
        note_period[i] = response[i];
      }
      song_to_play.note_period = atof(note_period);

      //get the notes and append to song_to_play.notes
      int num_notes = 0;
      char *token;
      token = strtok(notes, ",");  //get first token
      /* walk through other tokens */
      while( token != NULL ) {
        Serial.println(token);
        song_to_play.notes[num_notes] = atof(token);
        token = strtok(NULL, ",");
        num_notes++;
     }

     //get the length of the song, ie. number of notes
     song_to_play.length = num_notes;

     //play riff on loop; only stop after button1 is pushed
     int stop_ = 0;
     while (!stop_){

      //Iterate through each note in the song and play
      for (int i=0; i < song_to_play.length; i++){
        if (!digitalRead(BUTTON1)) { // Button 1 is pushed
          stop_ = 1;
        }
      
        new_note = song_to_play.notes[i];

        float play_time = millis();
        //only play note if new_note is different from old_note
        if (new_note != old_note){
          ledcWriteTone(AUDIO_PWM, new_note);
          //change color
          if (new_note > old_note){
            tft.fillScreen(TFT_ORANGE);
          } else {
            tft.fillScreen(TFT_BLUE);
          }
          char output[30] = "";
          sprintf(output, "Hz: %4.2f", new_note);
          tft.setCursor(0, 0, 2);
          tft.println(output);          
          while (millis() - play_time < song_to_play.note_period);
        
          old_note = new_note;
        }
       }

     state = 0;
     ledcWriteTone(AUDIO_PWM, 0);
     }
    }
  }
};

//SongCreator class and FSM for user input of songs
class SongCreator {
    char digits[50] = "0123456789";
    char msg[400] = {0}; //contains previous query response
    char query_string[50] = {0};
    int note_index;
    int state;
    uint32_t scroll_timer;
    const int scroll_threshold = 200;
    const float angle_threshold = 0.3;
  public:

    SongCreator() {
      state = 0;
      memset(msg, 0, sizeof(msg));//empty it.
      strcat(msg, "Long Press to input a single note's duration!");
      note_index = 0;
      scroll_timer = millis();
    }
    
    void update(float angle, int button, char* output) {
      
      if (state == 0){ // State 0 - Message Display
        strcpy(output, msg);
        if (button == 2){ // long button press state - reset variables
          state = 1;
          strcpy(query_string, "");
          note_index = 0;
          scroll_timer = millis();
        }
      }
      
      else if (state == 1){ // State 1 - Note Duration Entry
        while (millis() - last_read < READING_PERIOD);
        last_read = millis();

        // Get the user input of Note duration based on IMU x reading
        imu.readAccelData(imu.accelCount);
        x = imu.accelCount[0] * imu.aRes;
        float duration = abs(x*100);
        sprintf(output, "Note Duration: \n%4.2f", duration);
        
        // short button press to set the duration
        if (button == 1){
          song_to_create.note_period = duration;
          state = 2;
        }
      }

    else if (state == 2){ // State 2 - input notes
      while (millis() - last_read < READING_PERIOD);
      last_read = millis();
      imu.readAccelData(imu.accelCount);
      x = imu.accelCount[0] * imu.aRes;
      Serial.println("Acceleration x:");
      Serial.println(x);

      //allow for silences if x acceleration is between -0.1 and 0.1
      if (x >= -.1 and x <= .1){
          new_note = 0.0;
      }

      //otherwise find the angle thresholds
      else{
        for (int i=0; i < NOTE_COUNT; i++){
          if (x >= accel_thresholds[i] && x < accel_thresholds[i+1]){
            new_note = note_freqs[i];
            break;
          }
        }
      }
      Serial.println("new_note:\n");
      Serial.println(new_note);
    
      //only play note if new_note is different from old_note
      if (new_note != old_note){
        ledcWriteTone(AUDIO_PWM, new_note);
      }
      old_note = new_note;
      sprintf(output, "Hz: %4.2f", new_note);
      
      if (button == 1){ // short press, add to notes
        song_to_create.notes[note_index] = new_note;
        note_index++;
      }
      else if (button == 2){ // long press, move to next state
        state = 3;
        song_to_create.length = note_index - 1;
      }
    }
    
    else if (state == 3){ //State 3 - Format and send Query
      state = 4;
      sprintf(riff_format, "%4.2f&", song_to_create.note_period);

      char temp[50] = "";
      for (int i=0; i < song_to_create.length; i++){
        // don't add comma for last one
        if (song_to_create.length - i == 1){
          sprintf(temp, "%4.2f", song_to_create.notes[i]);
        }
        else{
          sprintf(temp, "%4.2f,", song_to_create.notes[i]);
        }
        strcat(riff_format, temp);
      }

      Serial.println("riff format:");
      Serial.println(riff_format);

      char body[100]; //for body
      sprintf(body,"{\"artist\": \"%s\", \"song\":\"%s\"}",USER,riff_format);//generate body, posting to User, 1 step
      int body_len = strlen(body); //calculate body length (for header reporting)
      sprintf(post_request_buffer,"POST /esp32test/limewire HTTP/1.1\r\n");
      strcat(post_request_buffer,"Host: iesc-s3.mit.edu\r\n");
      strcat(post_request_buffer,"Content-Type: application/json\r\n");
      sprintf(post_request_buffer+strlen(post_request_buffer),"Content-Length: %d\r\n", body_len); //append string formatted to end of request buffer
      strcat(post_request_buffer,"\r\n"); //new line from header to body
      strcat(post_request_buffer,body); //body
      strcat(post_request_buffer,"\r\n"); //new line
      Serial.println(post_request_buffer);
      do_http_request("iesc-s3.mit.edu", post_request_buffer, post_response, OUT_BUFFER_SIZE, RESPONSE_TIMEOUT,true);
  
      Serial.println("\nRESPONSE BUFFER:\n");
      Serial.println(post_response); //viewable in Serial Terminal
      ledcWriteTone(AUDIO_PWM, 0);
      sprintf(output, "Posting...\n%s \nLong press again to restart.", post_response);
    }
    else if (state == 4){ //Idle state: wait for long press to go back again
      if (button == 2){
        state = 0;
      }
    }
    
  }
};

SongIDGetter song_id_getter; //wikipedia object
SongCreator song_creator;


void loop() {
  
  float x, y;
  get_angle(&x, &y); //get angle values
  int bv1 = button1.update(); //get button value
  int bv2 = button2.update(); //get button value
  
  if (state==PLAYBACK){
    song_id_getter.update(y, bv1, response);
    if (bv2==2){ //button2 long press
      state = RECORD;
    }
  } else if (state==RECORD){
    song_creator.update(y, bv1, response);
    if (bv2==2){ //button2 long press
      state = PLAYBACK;
    }
  }

  if (strcmp(response, old_response) != 0) {//only draw if changed!
      tft.fillScreen(TFT_BLACK);
      tft.setCursor(0, 0, 2);
      tft.println(response);
    }
   memset(old_response, 0, sizeof(old_response));
   strcat(old_response, response);

  
  while (millis() - primary_timer < LOOP_PERIOD); //wait for primary timer to increment
  primary_timer = millis();
  
}

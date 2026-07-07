//Version 3.1 By Owen Sobel

//This program is used to control a robot using a app that communicates with Arduino through a bluetooth module.

//Error Code Chart: Code 01; Turnradius is higher than Speed; Code 02; Speed is higher than 255;

#define in1 5 //L298n Motor Driver pins.

#define in2 6

#define in3 10

#define in4 11

#define LED 13

int command; //Int to store app command state.

int Speed = 204; // 0 - 255.

int Speedsec;

int buttonState = 0;

int lastButtonState = 0;

int Turnradius = 0; //Set the radius of a turn, 0 - 255 Note:the robot will malfunction if this is higher than int Speed.

int brakeTime = 45;

int brakeTimeForTurn = brakeTime - 30;

int brkonoff = 1; //1 for the electronic braking system, 0 for normal.

void setup() {

pinMode(in1, OUTPUT);

pinMode(in2, OUTPUT);

pinMode(in3, OUTPUT);

pinMode(in4, OUTPUT);

pinMode(LED, OUTPUT); //Set the LED pin.

Serial.begin(9600); //Set the baud rate to your Bluetooth module.

}

void loop() {

if (Serial.available() > 0) {

command = Serial.read();

Stop(); //Initialize with motors stoped.

switch (command) {

case 'F':

forward(Speed);

break;

case 'B':

back(Speed);

break;

case 'L':

left(Speed);

break;

case 'R':

right(Speed);

break;

case 'G':

forwardleft(Speed, Speedsec);

break;

case 'I':

forwardright(Speed, Speedsec);

break;

case 'H':

backleft(Speed, Speedsec);

break;

case 'J':

backright(Speed, Speedsec);

break;

case '0':

Speed = 100;

break;

case '1':

Speed = 140;

break;

case '2':

Speed = 153;

break;

case '3':

Speed = 165;

break;

case '4':

Speed = 178;

break;

case '5':

Speed = 191;

break;

case '6':

Speed = 204;

break;

case '7':

Speed = 216;

break;

case '8':

Speed = 229;

break;

case '9':

Speed = 242;

break;

case 'q':

Speed = 255;

break;

}

Speedsec = Turnradius;

if (brkonoff == 1) {

brakeOn();

} else {

brakeOff();

}

}

}

int forward(int spd) {

analogWrite(in1, spd);

analogWrite(in3, spd);

}

int back(int spd) {

analogWrite(in2, spd);

analogWrite(in4, spd);

}

int left(int spd) {

analogWrite(in3, spd);

analogWrite(in2, spd);

}

int right(int spd) {

analogWrite(in4, spd);

analogWrite(in1, spd);

}

int forwardleft(int spd, int spdsc) {

analogWrite(in1, Speedsec);

analogWrite(in3, spd);

}

int forwardright(int spd, int spdsc) {

analogWrite(in1, spd);

analogWrite(in3, spdsc);

}

int backright(int spd, int spdsc) {

analogWrite(in2, spd);

analogWrite(in4, spdsc);

}

int backleft(int spd, int spdsc) {

analogWrite(in2, spdsc);

analogWrite(in4, spd);

}

void Stop() {

analogWrite(in1, 0);

analogWrite(in2, 0);

analogWrite(in3, 0);

analogWrite(in4, 0);

}

void brakeOn() {

//Here's the future use: an electronic braking system!

// read the pushbutton input pin:

buttonState = command;

// compare the buttonState to its previous state

if (buttonState != lastButtonState) {

// if the state has changed, increment the counter

if (lastButtonState == 'F') {

if (buttonState == 'S') {

back(Speed);

delay(brakeTime);

Stop();

}

}

if (lastButtonState == 'B') {

if (buttonState == 'S') {

forward(Speed);

delay(brakeTime);

Stop();

}

}

if (lastButtonState == 'L') {

if (buttonState == 'S') {

right(Speed);

delay(brakeTimeForTurn);

Stop();

}

}

if (lastButtonState == 'R') {

if (buttonState == 'S') {

left(Speed);

delay(brakeTimeForTurn);

Stop();

}

}

}

// save the current state as the last state,

//for next time through the loop

lastButtonState = buttonState;

}

void brakeOff() {

}

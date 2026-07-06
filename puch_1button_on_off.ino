/////////////////////////////////
// Generated with a lot of love//
// with TUNIOT FOR ESP32     //
// Website: Easycoding.tn      //
/////////////////////////////////
int  old_button;
int  button;
int  state;
void setup()
{
old_button = 0;
button = 0;
state = 0;
pinMode(21, INPUT);
Serial.begin(9600);
pinMode(4, OUTPUT);

}
void loop()
{
    button = digitalRead(21);
    Serial.println(digitalRead(21));
    if (old_button == 0 && button == 1) {
      state = 1 - state;
    }
    old_button = button;
    if (state == 1) {
      digitalWrite(4,HIGH);

    } else {
      digitalWrite(4,LOW);

    }
}
#include <Wire.h>
#include <PCF8574.h>
#include <string>

//motors
const int stepsPerRevolution = 2048; 
#define X_AXIS_MOTOR    1
#define Y_AXIS_MOTOR    2

const uint8_t stepSequence[4] = {
    0b0001, // A
    0b0010, // B
    0b0100, // C
    0b1000  // D
};

int stepIndex = 0;

//extender
TwoWire I2CBUS = TwoWire(0);
PCF8574 pcf8574(0x20, &I2CBUS);

String command = "";

void setup() {
  Serial.begin(115200);

  I2CBUS.begin(14, 15);

  if (pcf8574.begin())
      Serial.println("OK");
  else Serial.println("Failed");
}

void loop() {
  while (Serial.available()) {
    char incomingChar = Serial.read();
    if (incomingChar == '\n') {
      commandHandler(command);
      command = "";
    } else {
      command += incomingChar;
    }
  }
  Serial.println("Patapim br brr");
  delay(500);
}

void stepMotor(int steps, int motorNum) {
    int stepsAbs = abs(steps);
    for (int i = 0; i < stepsAbs; i++) {
        // wybór kroku
        if (steps >= 0) {
        stepIndex = (stepIndex + 1) % 4;
        } else {
        stepIndex = (stepIndex + 3) % 4;
        }

        if (motorNum == 1){
            pcf8574.write(0,  (stepSequence[stepIndex] >> 0) & 0x01);
            pcf8574.write(1,  (stepSequence[stepIndex] >> 1) & 0x01);
            pcf8574.write(2,  (stepSequence[stepIndex] >> 2) & 0x01);
            pcf8574.write(3,  (stepSequence[stepIndex] >> 3) & 0x01);
        }
        else if(motorNum == 2){
            pcf8574.write(4,  (stepSequence[stepIndex] >> 0) & 0x01);
            pcf8574.write(5,  (stepSequence[stepIndex] >> 1) & 0x01);
            pcf8574.write(6,  (stepSequence[stepIndex] >> 2) & 0x01);
            pcf8574.write(7,  (stepSequence[stepIndex] >> 3) & 0x01);
        }
        delay(2);
    }
}

void commandHandler(String command){
  Serial.println("");
  char engine = command[0];
  String error = "No such command: ";

  if(engine == 'X'){
    int steps = command.substring(2).toInt();
    Serial.print("X axis motor rotating: ");
    Serial.print(command.substring(2));
    Serial.print(" steps");
    stepMotor(steps, X_AXIS_MOTOR);
  }
  else if(engine == 'Y'){
    int steps = command.substring(2).toInt();
    Serial.print("Y axis motor rotating: ");
    Serial.print(command.substring(2));
    Serial.print(" steps");
    stepMotor(steps, Y_AXIS_MOTOR);
  }
  else{
    error += engine;
    Serial.println(error);
  }

}



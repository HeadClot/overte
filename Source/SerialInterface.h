//
//  SerialInterface.h
//  


#ifndef interface_SerialInterface_h
#define interface_SerialInterface_h

#include "glm.hpp"
#include "util.h"
#include "world.h"
#include <GLUT/glut.h>
#include <iostream>

// These includes are for serial port reading/writing
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

#define NUM_CHANNELS 6

//  Acceleration sensors, in screen/world coord system (X = left/right, Y = Up/Down, Z = fwd/back)
#define ACCEL_X 4 
#define ACCEL_Y 5 
#define ACCEL_Z 3 

//  Gyro sensors, in coodinate system of head/airplane
#define PITCH_RATE 0 
#define YAW_RATE 1 
#define ROLL_RATE 2

class SerialInterface {
public:
    int init(char * portname, int baud);
    void readData();
    int getLED() {return LED;};
    int getNumSamples() {return samplesAveraged;};
    int getValue(int num) {return lastMeasured[num];};
    int getRelativeValue(int num) {return lastMeasured[num] - trailingAverage[num];};
    float getTrailingValue(int num) {return trailingAverage[num];};
    void resetTrailingAverages();
    void renderLevels(int width, int height);
private:
    int lastMeasured[NUM_CHANNELS];
    float trailingAverage[NUM_CHANNELS];
    int samplesAveraged;
    int LED;
};

#endif

//--------------------------------------------------------------
//-- Oscillator.pde
//-- Generate sinusoidal oscillations in the servos
//--------------------------------------------------------------
//-- (c) Juan Gonzalez-Gomez (Obijuan), Dec 2011
//-- GPL license
//--------------------------------------------------------------
#if defined(ARDUINO) && ARDUINO >= 100
  #include "Arduino.h"
#else
  #include "WProgram.h"
  #include <pins_arduino.h>
#endif
#include "Oscillator.h"

//-- This function returns true if another sample
//-- should be taken (i.e. the TS time has passed since
//-- the last sample was taken
bool Oscillator::next_sample()
{
  
  //-- Read current time
  _currentMillis = millis();
 
  //-- Check if the timeout has passed
  if(_currentMillis - _previousMillis > _samplingPeriod) {
    _previousMillis = _currentMillis;   

    return true;
  }
  
  return false;
}

//-- Attach an oscillator to a servo
//-- Input: pin is the arduino pin were the servo
//-- is connected
void Oscillator::attach(int pin, bool rev)
{
  //-- If the oscillator is detached, attach it.
  if(!_servo.attached()){

    //-- Attach the servo and move it to the home position
      _servo.attach(pin);
      _pos = 90; 
      _servo.write(90);
      _previousServoCommandMillis = millis();

      //-- Initialization of oscilaltor parameters
      _samplingPeriod=30;
      _period=2000;
      _numberSamples = _period/_samplingPeriod;
      _inc = 2*M_PI/_numberSamples;

      _previousMillis=0;

      //-- Default parameters
      _amplitude=45;
      _phase=0;
      _phase0=0;
      _offset=0;
      _stop=false;

      //-- Reverse mode
      _rev = rev;
  }
      
}

//-- Detach an oscillator from his servo
void Oscillator::detach()
{
   //-- If the oscillator is attached, detach it.
  if(_servo.attached())
        _servo.detach();

}

/*************************************/
/* Set the oscillator period, in ms  */
/*************************************/
void Oscillator::SetT(unsigned int T)
{
  //-- Assign the new period
  _period=T;
  
  //-- Recalculate the parameters
  _numberSamples = _period/_samplingPeriod;
  _inc = 2*M_PI/_numberSamples;
};

/*******************************/
/* Manual set of the position  */
/******************************/

void Oscillator::SetPosition(int position)
{
  write(position);
};


/*******************************************************************/
/* This function should be periodically called                     */
/* in order to maintain the oscillations. It calculates            */
/* if another sample should be taken and position the servo if so  */
/*******************************************************************/
// [4] 振荡器刷新：每隔 _samplingPeriod(约30ms) 采样一次正弦并更新舵机
void Oscillator::refresh()
{
  
  if (next_sample()) {             // 距上次采样是否已过约 30ms
  
      if (!_stop) {                // 未暂停时才输出新角度
         // 相对中位 90° 的偏移 = A*sin(相位+初相) + O
         int pos = round(_amplitude * sin(_phase + _phase0) + _offset);
	       if (_rev) pos=-pos;     // 反向安装时翻转符号
         write(pos+90);            // 转成绝对角 0~180 并写入（含限速）
      }

      _phase = _phase + _inc;      // 相位前进一小步，下一采样点沿正弦前移

  }
}

// [5] 把目标角度写到舵机：可限速，最后 PWM 输出
void Oscillator::write(int position) 
{
  long currentMillis = millis();
  if (_diff_limit > 0) {           // 若启用了度/秒限速（enableServoLimit）
    // 根据距上次命令的时间，算本步允许的最大角度变化
    int limit =  max(1,(((int)(currentMillis - _previousServoCommandMillis)) * _diff_limit) / 1000);
    if (abs(position - _pos) > limit) {
      _pos += position < _pos ? -limit : limit;  // 目标太远则只走 limit 那么多
    } else {
      _pos = position;             // 在允许范围内则直接到位
    }
  }
  else {
      _pos = position;             // 无限速则直接采用目标角
  }    
  _previousServoCommandMillis = currentMillis;
  _servo.write(_pos + _trim);      // 加校准偏移后输出 PWM（开环，无反馈）
}

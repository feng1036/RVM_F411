#include "rmp.h"
#include "rvm.h"
#include <stdio.h>
#include "sensor_task.h"
#include "math.h"
#include"stm32f4xx.h"
static bool gyroBiasFound=false;
static bool isBaroPresent=false;
static float accScaleSum=0;
static float accScale=1;
static enum {IDLE, BMP280}baroType = IDLE;
static bool isInit=false;

#define SENSOR_MSG ((volatile sensorData_t*)DATA_SHARED_SENSOR_BASE)
/*�������жϳ�ʼ��*/
static void sensorsInterruptInit(void)
{
    /*ʹ��MPU6500�ж�*/
    GPIOA->MODER&=~(GPIO_MODER_MODER4);
    GPIOA->PUPDR&=~(GPIO_PUPDR_PUPDR4);
    GPIOA->PUPDR|=(uint32_t)(0x200);
    SYSCFG->EXTICR[1]&=~(uint32_t)0x0F;
    EXTI->IMR&=~(uint32_t)0x00010;
    EXTI->EMR&=~(uint32_t)0x00010;
    EXTI->IMR|=(uint32_t)0x00010;
    EXTI->RTSR&=~(uint32_t)0x00010;
    EXTI->FTSR&=~(uint32_t)0x00010;
    EXTI->RTSR|=(uint32_t)0x00010;
    RMP_INT_MASK();
    EXTI->PR=(uint32_t)0x00010;
    RMP_INT_UNMASK();
}
/* ������������ʼ�� */
void sensorsDeviceInit(void)
{
    I2Cdev_Init(I2C1_Dev);
    MPU6500_Init(I2C1_DEV);
    for (int i=0;i<3;i++)//��ʼ�����ټƺ����ݶ��׵�ͨ�˲�
    {
        lpf2pInit(&gyroLpf[i],1000,80);
        lpf2pInit(&accLpf[i],1000,30);
    }
    if (BMP280_Init(I2C1_DEV) == true)//BMP280��ʼ��
    {
        isBaroPresent = true;
        baroType = BMP280;
        //vTaskDelay(100);
        RMP_Thd_Delay(1000);
    }
    else
    {
        isBaroPresent = false;
    }
}
/*������ƫ�ó�ʼ��*/
static void sensorsBiasObjInit(BiasObj* bias)
{
    bias->isBufferFilled = false;
    bias->bufHead = bias->buffer;
}

/*���㷽���ƽ��ֵ*/
static void sensorsCalculateVarianceAndMean(BiasObj* bias, Axis3f* varOut, Axis3f* meanOut)
{
    u32 i;
    int64_t sum[3] = {0};
    int64_t sumsq[3] = {0};
    for (i = 0; i < SENSORS_NBR_OF_BIAS_SAMPLES; i++)
    {
        sum[0]+= bias->buffer[i].x;
        sum[1]+= bias->buffer[i].y;
        sum[2]+= bias->buffer[i].z;
        sumsq[0]+= bias->buffer[i].x * bias->buffer[i].x;
        sumsq[1]+= bias->buffer[i].y * bias->buffer[i].y;
        sumsq[2]+= bias->buffer[i].z * bias->buffer[i].z;
    }
    varOut->x = (sumsq[0] - ((int64_t)sum[0] * sum[0]) / SENSORS_NBR_OF_BIAS_SAMPLES);
    varOut->y = (sumsq[1] - ((int64_t)sum[1] * sum[1]) / SENSORS_NBR_OF_BIAS_SAMPLES);
    varOut->z = (sumsq[2] - ((int64_t)sum[2] * sum[2]) / SENSORS_NBR_OF_BIAS_SAMPLES);
    meanOut->x = (float)sum[0] / SENSORS_NBR_OF_BIAS_SAMPLES;
    meanOut->y = (float)sum[1] / SENSORS_NBR_OF_BIAS_SAMPLES;
    meanOut->z = (float)sum[2] / SENSORS_NBR_OF_BIAS_SAMPLES;
}
/*����������ƫ��ֵ*/
static bool sensorsFindBiasValue(BiasObj* bias)
{
    bool foundbias = false;
    if (bias->isBufferFilled)
    {
        Axis3f mean;
        Axis3f variance;
        sensorsCalculateVarianceAndMean(bias, &variance, &mean);
        if (variance.x < GYRO_VARIANCE_BASE && variance.y < GYRO_VARIANCE_BASE && variance.z < GYRO_VARIANCE_BASE)
        {
            bias->bias.x = mean.x;
            bias->bias.y = mean.y;
            bias->bias.z = mean.z;
            foundbias = true;
            bias->isBiasValueFound= true;
        }
        else
        {
            bias->isBufferFilled=false;
        }
    }
    return foundbias;
}

/* ��������ʼ�� */
void Sensor_Init(void)
{
    if(isInit) return;
    sensorsBiasObjInit(&gyroBiasRunning);
    sensorsDeviceInit();/*������������ʼ��*/
    sensorsInterruptInit();/*�������жϳ�ʼ��*/
    isInit = true;
}
/**
 * �����������ѭ�������������һ����ֵ�������������滻�ɵĵ�ֵ
 */
static void sensorsAddBiasValue(BiasObj* bias, int16_t x, int16_t y, int16_t z)
{
    bias->bufHead->x = x;
    bias->bufHead->y = y;
    bias->bufHead->z = z;
    bias->bufHead++;
    if (bias->bufHead >= &bias->buffer[SENSORS_NBR_OF_BIAS_SAMPLES])
    {
        bias->bufHead = bias->buffer;
        bias->isBufferFilled = true;
    }
}

/**
 * �������������������ٶ���������
 */
static bool processAccScale(int16_t ax, int16_t ay, int16_t az)
{
    static bool accBiasFound = false;
    static uint32_t accScaleSumCount = 0;
    if (!accBiasFound)
    {
        accScaleSum += sqrtf(powf(ax * SENSORS_G_PER_LSB_CFG, 2) + powf(ay * SENSORS_G_PER_LSB_CFG, 2) + powf(az * SENSORS_G_PER_LSB_CFG, 2));
        accScaleSumCount++;
        if (accScaleSumCount == SENSORS_ACC_SCALE_SAMPLES)
        {
            accScale = accScaleSum / SENSORS_ACC_SCALE_SAMPLES;
            accBiasFound = true;
        }
    }
    return accBiasFound;
}

/**
 * �������ݷ���
 */
static bool processGyroBias(int16_t gx, int16_t gy, int16_t gz, Axis3f *gyroBiasOut)
{
    sensorsAddBiasValue(&gyroBiasRunning, gx, gy, gz);
    if (!gyroBiasRunning.isBiasValueFound)
    {
        sensorsFindBiasValue(&gyroBiasRunning);
    }
    gyroBiasOut->x = gyroBiasRunning.bias.x;
    gyroBiasOut->y = gyroBiasRunning.bias.y;
    gyroBiasOut->z = gyroBiasRunning.bias.z;
    return gyroBiasRunning.isBiasValueFound;
}
/*������ѹ������*/
void processBarometerMeasurements(const u8 *buffer)
{
    static float temp;
    static float pressure;
    if ((buffer[0] & 0x08)) /*bit3=1 ת�����*/
    {
        s32 rawPressure = (s32)((((u32)(buffer[1])) << 12) | (((u32)(buffer[2])) << 4) | ((u32)buffer[3] >> 4));
        s32 rawTemp = (s32)((((u32)(buffer[4])) << 12) | (((u32)(buffer[5])) << 4) | ((u32)buffer[6] >> 4));
        temp = bmp280CompensateT(rawTemp)/100.0f;        
        pressure = bmp280CompensateP(rawPressure)/25600.0f;            
        sensors.baro.pressure = pressure;
        sensors.baro.temperature = (float)temp;/*��λ��*/
        sensors.baro.asl=bmp280PressureToAltitude(&pressure) * 100.f;/*ת���ɺ���*/
    }
}
/*������ټƺ�����������*/
void processAccGyroMeasurements(const uint8_t *buffer)
{
    /*ע�⴫������ȡ����(��ת270��x��y����)*/
    int16_t ay = (((int16_t) buffer[0]) << 8) | buffer[1];
    int16_t ax = ((((int16_t) buffer[2]) << 8) | buffer[3]);
    int16_t az = (((int16_t) buffer[4]) << 8) | buffer[5];
    int16_t gy = (((int16_t) buffer[8]) << 8) | buffer[9];
    int16_t gx = (((int16_t) buffer[10]) << 8) | buffer[11];
    int16_t gz = (((int16_t) buffer[12]) << 8) | buffer[13];
    gyroBiasFound = processGyroBias(gx, gy, gz, &gyroBias);
    if (gyroBiasFound)
    {
        processAccScale(ax, ay, az);    /*����accScale*/
    }
    sensors.gyro.x = -(gx - gyroBias.x) * SENSORS_DEG_PER_LSB_CFG;    /*��λ ��/s */
    sensors.gyro.y =  (gy - gyroBias.y) * SENSORS_DEG_PER_LSB_CFG;
    sensors.gyro.z =  (gz - gyroBias.z) * SENSORS_DEG_PER_LSB_CFG;
    applyAxis3fLpf(gyroLpf, &sensors.gyro);    
    sensors.acc.x = -(ax) * SENSORS_G_PER_LSB_CFG / accScale;    /*��λ g(9.8m/s^2)*/
    sensors.acc.y =  (ay) * SENSORS_G_PER_LSB_CFG / accScale;    /*�������ٶ���������accScale ������������ó�*/
    sensors.acc.z =  (az) * SENSORS_G_PER_LSB_CFG / accScale;
    applyAxis3fLpf(accLpf, &sensors.acc);
}
/*����������*/
void Sensor_Task(void)
{
    Sensor_Init();    /*��������ʼ��*/
    //vTaskDelay(150);
    RMP_Thd_Delay(1500);
    sensorsSetupSlaveRead();/*���ô�������ģʽ��ȡ*/
    while (1)
    {
        /*ȷ�����ݳ���*/
        uint8_t dataLen = (uint8_t)(SENSORS_MPU6500_BUFF_LEN +SENSORS_BARO_BUFF_LEN);
        i2cdevRead(I2C1_DEV, MPU6500_ADDRESS_AD0_HIGH, MPU6500_RA_ACCEL_XOUT_H, dataLen, buffer);
        /*����ԭʼ���ݣ����������ݶ�����*/
        processAccGyroMeasurements(&(buffer[0]));
        processBarometerMeasurements(&(buffer[SENSORS_MPU6500_BUFF_LEN]));
        *SENSOR_MSG=sensors;
        RVM_Hyp_Evt_Snd(11);
        //ÿ1ms����һ������
        RMP_Thd_Delay(10);
    }    
}

/*���׵�ͨ�˲�*/
static void applyAxis3fLpf(lpf2pData *data,Axis3f* in)
{
    for (u8 i = 0; i < 3; i++)
    {
        in->axis[i]=lpf2pApply(&data[i], in->axis[i]);
    }
}

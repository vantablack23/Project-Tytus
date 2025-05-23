#include <ProjectTytus_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include <Preferences.h>
#include "esp_camera.h"
#include <Wire.h>
#include <PCF8574.h>

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
int prevX = 48;
int prevY = 48;

//extender
TwoWire I2CBUS = TwoWire(0);
PCF8574 pcf8574(0x20, &I2CBUS);

//zapamietywanie pozycji
Preferences preferences;

//camera
#define CAMERA_MODEL_AI_THINKER // Has PSRAM

#if defined(CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#else
#error "Camera model not selected"
#endif

/* Constant defines -------------------------------------------------------- */
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS           320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS           240
#define EI_CAMERA_FRAME_BYTE_SIZE                 3

/* Private variables ------------------------------------------------------- */
static bool debug_nn = false; // Set this to true to see e.g. features generated from the raw signal
static bool is_initialised = false;
uint8_t *snapshot_buf; //points to the output of the capture

static camera_config_t camera_config = {
    .pin_pwdn = PWDN_GPIO_NUM,
    .pin_reset = RESET_GPIO_NUM,
    .pin_xclk = XCLK_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl = SIOC_GPIO_NUM,

    .pin_d7 = Y9_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM,
    .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href = HREF_GPIO_NUM,
    .pin_pclk = PCLK_GPIO_NUM,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG, //YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_QVGA,    //QQVGA-UXGA Do not use sizes above QVGA when not JPEG

    .jpeg_quality = 12, //0-63 lower number means higher quality
    .fb_count = 1,       //if more than one, i2s runs in continuous mode. Use only with JPEG
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

/* Function definitions ------------------------------------------------------- */
bool ei_camera_init(void);
void ei_camera_deinit(void);
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) ;

/**
* @brief      Arduino setup function
*/
void setup()
{
    // put your setup code here, to run once:
    Serial.begin(115200);
    delay(5000);
    //comment out the below line to start inference immediately after upload
    while (!Serial);
    Serial.println("Edge Impulse Inferencing Demo");
    if (ei_camera_init() == false) {
        ei_printf("Failed to initialize Camera!\r\n");
    }
    else {
        ei_printf("Camera initialized\r\n");
    }

    ei_printf("\nStarting continious inference in 2 seconds...\n");
    ei_sleep(2000);

    I2CBUS.begin(14, 15);

    if (pcf8574.begin())
        Serial.println("OK");
    else Serial.println("Failed");

    preferences.begin("enginesPos", false);
    if(preferences.isKey("xAxisMotor") == false){
        Serial.println("xAxisMotor INIT");
        preferences.putInt("xAxisMotor", 0);
    }
    if(preferences.isKey("yAxisMotor") == false){
        Serial.println("YAxisMotor INIT");
        preferences.putInt("yAxisMotor", 0);
    }
    Serial.println("Resetting motors positions...");

    Serial.println(preferences.getInt("xAxisMotor"));
    Serial.println(preferences.getInt("yAxisMotor"));

    resetMotorPosition(X_AXIS_MOTOR, preferences.getInt("xAxisMotor"));
    resetMotorPosition(Y_AXIS_MOTOR, preferences.getInt("yAxisMotor"));
    Serial.println("Resetting positions done.");
    // stepMotor(X_AXIS_MOTOR, 2048);
    // stepMotor(Y_AXIS_MOTOR, 2048);
    Serial.printf("Model input size: %dx%d\n", EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);

    aim(0, 96, 48, 48);
    prevX = 0;
    prevY = 96;
    delay(2000);
    aim(0, 0, prevX, prevY);
    prevX = 0;
    prevY = 0;
    delay(2000);
    aim(48, 0, prevX, prevY);
    prevX = 48;
    prevY = 0;
    delay(2000);
    aim(96, 0, prevX, prevY);
    prevX = 96;
    prevY = 0;
    delay(2000);
    aim(48, 48, prevX, prevY);
    prevX = 48;
    prevY = 48;
    delay(2000);
}

/**
* @brief      Get data and run inferencing
*
* @param[in]  debug  Get debug info if true
*/
void loop()
{

    // instead of wait_ms, we'll wait on the signal, this allows threads to cancel us...
    if (ei_sleep(5) != EI_IMPULSE_OK) {
        return;
    }

    snapshot_buf = (uint8_t*)malloc(EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);

    // check if allocation was successful
    if(snapshot_buf == nullptr) {
        ei_printf("ERR: Failed to allocate snapshot buffer!\n");
        return;
    }

    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = &ei_camera_get_data;

    if (ei_camera_capture((size_t)EI_CLASSIFIER_INPUT_WIDTH, (size_t)EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf) == false) {
        ei_printf("Failed to capture image\r\n");
        free(snapshot_buf);
        return;
    }

    // Run the classifier
    ei_impulse_result_t result = { 0 };

    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", err);
        return;
    }

    // print the predictions
    ei_printf("Predictions (DSP: %d ms., Classification: %d ms., Anomaly: %d ms.): \n",
                result.timing.dsp, result.timing.classification, result.timing.anomaly);

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    ei_printf("Object detection bounding boxes:\r\n");
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value == 0) {
            continue;
        }
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,

                bb.height);
        // aim(bb.x+(bb.width/2), bb.y+(bb.height/2), prevX, prevY);
        // prevX = bb.x+(bb.width/2);
        // prevY = bb.y+(bb.height/2);
        // stepMotor(X_AXIS_MOTOR, 500);
        // stepMotor(Y_AXIS_MOTOR, -500);

        aim(bb.x, bb.y+bb.height, prevX, prevY);
        prevX = bb.x;
        prevY = bb.y+bb.height;

        delay(500);
    }

    // Print the prediction results (classification)
#else
    ei_printf("Predictions:\r\n");
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        ei_printf("  %s: ", ei_classifier_inferencing_categories[i]);
        ei_printf("%.5f\r\n", result.classification[i].value);
    }
#endif

    // Print anomaly result (if it exists)
#if EI_CLASSIFIER_HAS_ANOMALY
    ei_printf("Anomaly prediction: %.3f\r\n", result.anomaly);
#endif

#if EI_CLASSIFIER_HAS_VISUAL_ANOMALY
    ei_printf("Visual anomalies:\r\n");
    for (uint32_t i = 0; i < result.visual_ad_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.visual_ad_grid_cells[i];
        if (bb.value == 0) {
            continue;
        }
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,
                bb.height);
    }
#endif


    free(snapshot_buf);

}

/**
 * @brief   Setup image sensor & start streaming
 *
 * @retval  false if initialisation failed
 */
bool ei_camera_init(void) {

    if (is_initialised) return true;

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
      Serial.printf("Camera init failed with error 0x%x\n", err);
      return false;
    }

    sensor_t * s = esp_camera_sensor_get();
    // initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1); // flip it back
      s->set_brightness(s, 1); // up the brightness just a bit
      s->set_saturation(s, 0); // lower the saturation
    }

#if defined(CAMERA_MODEL_M5STACK_WIDE)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
#elif defined(CAMERA_MODEL_ESP_EYE)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
    s->set_awb_gain(s, 1);
#endif

    is_initialised = true;
    return true;
}

/**
 * @brief      Stop streaming of sensor data
 */
void ei_camera_deinit(void) {

    //deinitialize the camera
    esp_err_t err = esp_camera_deinit();

    if (err != ESP_OK)
    {
        ei_printf("Camera deinit failed\n");
        return;
    }

    is_initialised = false;
    return;
}


/**
 * @brief      Capture, rescale and crop image
 *
 * @param[in]  img_width     width of output image
 * @param[in]  img_height    height of output image
 * @param[in]  out_buf       pointer to store output image, NULL may be used
 *                           if ei_camera_frame_buffer is to be used for capture and resize/cropping.
 *
 * @retval     false if not initialised, image captured, rescaled or cropped failed
 *
 */
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
    bool do_resize = false;

    if (!is_initialised) {
        ei_printf("ERR: Camera is not initialized\r\n");
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
        ei_printf("Camera capture failed\n");
        return false;
    }

   bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);

   esp_camera_fb_return(fb);

   if(!converted){
       ei_printf("Conversion failed\n");
       return false;
   }

    if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS)
        || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) {
        do_resize = true;
    }

    if (do_resize) {
        ei::image::processing::crop_and_interpolate_rgb888(
        out_buf,
        EI_CAMERA_RAW_FRAME_BUFFER_COLS,
        EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
        out_buf,
        img_width,
        img_height);
    }


    return true;
}

static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr)
{
    // we already have a RGB888 buffer, so recalculate offset into pixel index
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;

    while (pixels_left != 0) {
        // Swap BGR to RGB here
        // due to https://github.com/espressif/esp32-camera/issues/379
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16) + (snapshot_buf[pixel_ix + 1] << 8) + snapshot_buf[pixel_ix];

        // go to the next pixel
        out_ptr_ix++;
        pixel_ix+=3;
        pixels_left--;
    }
    // and done!
    return 0;
}

void stepMotor(int motorNum, int steps) {
    int stepsAbs = abs(steps);
    int currentMotorPosition = 0;
    if (motorNum == 1){
      currentMotorPosition = preferences.getInt("xAxisMotor");
    }
    else if(motorNum == 2){
      currentMotorPosition = preferences.getInt("yAxisMotor");
    }
    for (int i = 0; i < stepsAbs; i++) {
        // wybór kroku
        if (steps >= 0) {
            stepIndex = (stepIndex + 1) % 4;
            currentMotorPosition++;
        } else {
            stepIndex = (stepIndex + 3) % 4;
            currentMotorPosition--;
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
    if (motorNum == 1){
        preferences.putInt("xAxisMotor", currentMotorPosition);
    }
    else if(motorNum == 2){
        preferences.putInt("yAxisMotor", currentMotorPosition);
    }
}

void resetMotorPosition(int motorNum, int steps){
  if(steps >= 0){
    stepMotor(motorNum, -steps);
  }
  else{
    stepMotor(motorNum, abs(steps));
  }
}

#define distance 128.00
#define xMid 48
#define yMid 48
#define stepsToMultiply 5.69

// void aim(int currX, int currY, int prevX, int prevY){
//     //OŚ X
//     float currDistX = float(currX-xMid); //dystans miedzy wykrytym punktem, a punktem środka obrazu
//     float prevDistX = float(prevX-xMid);

//     float currAngleX = atan(abs(currDistX)/distance)*180.00/M_PI; //wyliczenie kątó obu trójkątów
//     float prevAngleX = atan(abs(prevDistX)/distance)*180.00/M_PI;

//     float finalAngleX;
//     int stepsX;

//     if((currX < xMid && prevX > xMid) || (currX > xMid && prevX < xMid)){ //dodanie lub odjęcie kątów
//         finalAngleX = currAngleX + prevAngleX;
//     }
//     else{
//         finalAngleX = abs(currAngleX - prevAngleX);
//     }

//     stepsX = int(round(finalAngleX*stepsToMultiply));
//     if(prevX > currX){  //kierunek obrotu
//         stepsX = stepsX*-1;
//     }

//     stepMotor(X_AXIS_MOTOR, stepsX);

//     X

//     //OŚ Y
//     float currDistY = float(currY-yMid); //dystans miedzy wykrytym punktem, a punktem środka obrazu
//     float prevDistY = float(prevY-yMid);

//     float currAngleY = atan(abs(currDistY)/distance)*180.00/M_PI; //wyliczenie kątó obu trójkątów
//     float prevAngleY = atan(abs(prevDistY)/distance)*180.00/M_PI;

//     float finalAngleY;
//     int stepsY;

//     if((currY < yMid && prevY > yMid) || (currY > yMid && prevY < yMid)){ //dodanie lub odjęcie kątów
//         finalAngleY = currAngleY + prevAngleY;
//     }
//     else{
//         finalAngleY = abs(currAngleY - prevAngleY);
//     }


//     stepsY = int(round(finalAngleY*stepsToMultiply));
//     if(prevY < currY){  //kierunek obrotu
//         stepsY = stepsY*-1;
//     }

//     stepMotor(Y_AXIS_MOTOR, stepsY);
// }

void aim(int currX, int currY, int prevX, int prevY){
    if(currX!=prevX){
        resetMotorPosition(X_AXIS_MOTOR, preferences.getInt("xAxisMotor"));
    }
    if(currY!=prevY){
        resetMotorPosition(Y_AXIS_MOTOR, preferences.getInt("yAxisMotor"));
    }
    //OŚ X
    float currDistX = abs(float(currX-xMid)); //dystans miedzy wykrytym punktem, a punktem środka obrazu
    //float prevDistX = float(prevX-xMid);

    float currAngleX = atan(abs(currDistX)/distance)*180.00/M_PI; //wyliczenie kątó obu trójkątów
    //float prevAngleX = atan(abs(prevDistX)/distance)*180.00/M_PI;

    float finalAngleX=currAngleX;
    int stepsX;

    // if((currX < xMid && prevX > xMid) || (currX > xMid && prevX < xMid)){ //dodanie lub odjęcie kątów
    //     finalAngleX = currAngleX + prevAngleX;
    // }
    // else{
    //     finalAngleX = abs(currAngleX - prevAngleX);
    // }

    stepsX = int(round(finalAngleX*stepsToMultiply));
    if(currX<48){  //kierunek obrotu
        stepsX = stepsX*-1;
    }

    if(currX!=prevX){
        stepMotor(X_AXIS_MOTOR, stepsX);
    }

    float przeciwProstX=sqrt((distance*distance)+(currDistX*currDistX));

    //OŚ Y
    float currDistY = float(currY-yMid); //dystans miedzy wykrytym punktem, a punktem środka obrazu
    //float prevDistY = float(prevY-yMid);

    float currAngleY = atan(abs(currDistY)/przeciwProstX)*180.00/M_PI; //wyliczenie kątó obu trójkątów
    //float prevAngleY = atan(abs(prevDistY)/distance)*180.00/M_PI;

    float finalAngleY = currAngleY;
    int stepsY;
    Serial.println(finalAngleY);
    // if((currY < yMid && prevY > yMid) || (currY > yMid && prevY < yMid)){ //dodanie lub odjęcie kątów
    //     finalAngleY = currAngleY + prevAngleY;
    // }
    // else{
    //     finalAngleY = abs(currAngleY - prevAngleY);
    // }


    stepsY = int(round(finalAngleY*stepsToMultiply));
    if(currY>=48){  //kierunek obrotu
        stepsY = stepsY*-1;
    }

    if(currY!=prevY){
        stepMotor(Y_AXIS_MOTOR, stepsY);
    }
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor"
#endif

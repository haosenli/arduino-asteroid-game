#include <Arduino_FreeRTOS.h>
#include <LiquidCrystal.h>
#include <queue.h>
#include <semphr.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LED_PIN 49
#define LED_MATRIX_LEN 8
#define FRAME_BUFFER_LEN (LED_MATRIX_LEN - 1)
#define ANALOG_READ_SCALE 128
#define STARTING_MSPT 500.0
#define DIFFICULTY_DELAY 5000
#define MSPT_SCALING 1.2
#define LIGHT_CHANCE 20
// LCD
#define D4 52
#define D5 50
#define D6 48
#define D7 46
#define E 44
#define RS 42

// Begin adapted code from Ishaan Bhimani

#define OP_DECODEMODE 8
#define OP_SCANLIMIT 10
#define OP_SHUTDOWN 11
#define OP_DISPLAYTEST 14
#define OP_INTENSITY 10

// Transfers 1 SPI command to LED Matrix for given row
// Input: row - row in LED matrix
//       data - bit representation of LEDs in a given row; 1 indicates ON, 0
//       indicates OFF
void spiTransfer(volatile byte row, volatile byte data);

// change these pins as necessary
/// Arduino pin for the DIN pin on the 8x8 LED Matrix
int DIN = 10;
/// Arduino pin for the CS pin on the 8x8 LED Matrix
int CS = 11;
/// Arduino pin for the CLK pin on the 8x8 LED Matrix
int CLK = 12;
/// stores spi data
byte spidata[2]; // spi shift register uses 16 bits, 8 for ctrl and 8 for data

void spiTransfer(volatile byte opcode, volatile byte data)
{
    int offset = 0;   // only 1 device
    int maxbytes = 2; // 16 bits per SPI command

    for (int i = 0; i < maxbytes; i++)
    { // zero out spi data
        spidata[i] = (byte)0;
    }
    // load in spi data
    spidata[offset + 1] = opcode + 1;
    spidata[offset] = data;
    digitalWrite(CS, LOW); //
    for (int i = maxbytes; i > 0; i--)
        shiftOut(DIN, CLK, MSBFIRST,
                 spidata[i - 1]); // shift out 1 byte of data starting with
                                  // leftmost bit
    digitalWrite(CS, HIGH);
}

// End adapted code from Ishaan Bhimani

/// Stores the joystick location information.
int joystick_loc;
/// Stores the time between each tick in milliseconds
double mspt = STARTING_MSPT;

/// A structure that represents a frame buffer for the 8x8 LED matrix.
typedef struct framebuffer
{
    SemaphoreHandle_t mutex;
    byte buffer[FRAME_BUFFER_LEN];
    unsigned int current_head_row;
} FrameBuffer;

/// Stores the frame buffer struct
FrameBuffer fb;
/// Stores the information that gets sent through score_signal
int dummy;
/// Used for signaling that the game is over
QueueHandle_t score_signal = xQueueCreate(1, sizeof(int));
/// Used for setting up the liquid crystal display.
LiquidCrystal lcd(RS, E, D4, D5, D6, D7);

/**
 * @brief Flashes an off board LED in a sequence.
 *
 * Turns on an external LED for 100ms and off for 200ms.
 *
 * @param *pvParameters A parameter used by FreeRTOS when calling with @see xTaskCreate.
 * @return None.
 */
void task_rt1(void *pvParameters)
{
    pinMode(LED_PIN, OUTPUT);
    while (1)
    {
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief Sets up the given framebuffer.
 *
 * Takes in a framebuffer that is shared between other tasks and sets it up.
 *
 * @param fb A framebuffer struct shared between other tasks.
 * @return None.
 */
void fb_init(struct framebuffer *fb)
{
    fb->mutex = xSemaphoreCreateMutex();
    if (fb->mutex == NULL)
    {
        vTaskEndScheduler();
    }
    for (int i = 0; i < FRAME_BUFFER_LEN; i++)
    {
        fb->buffer[i] = 0;
    }
    fb->current_head_row = 0;
}

/**
 * @brief Writes a line to a given framebuffer.
 *
 * Writes line to the framebuffer @p fb. The framebuffer is shifted and the
 * least-recently written line is evicted.
 *
 * @param fb A framebuffer struct shared between other tasks.
 * @param line the line to add to the framebuffer.
 * @return None.
 */
void fb_write_line(struct framebuffer *fb, byte line)
{
    xSemaphoreTake(fb->mutex, portMAX_DELAY);
    if (fb->current_head_row == 0)
    {
        fb->current_head_row = FRAME_BUFFER_LEN - 1;
    }
    else
    {
        fb->current_head_row--;
    }
    fb->buffer[fb->current_head_row] = line;
    xSemaphoreGive(fb->mutex);
}

/**
 * @brief Copies contents of the given framebuffer to the given buffer.
 *
 * Copies the contents of framebuffer fb to buffer. The contents are copied
 * starting from the most recently written line.
 *
 * @param fb A framebuffer struct shared between other tasks.
 * @param buffer The buffer that framebuffer contents will be copied to.
 * @return None.
 */
void fb_read(struct framebuffer *fb, byte buffer[FRAME_BUFFER_LEN])
{
    xSemaphoreTake(fb->mutex, portMAX_DELAY);

    int buffer_index = 0;
    int current_head_row = fb->current_head_row;
    for (int i = current_head_row; i < FRAME_BUFFER_LEN; i++)
    {
        buffer[buffer_index] = fb->buffer[i];
        buffer_index++;
    }
    for (int i = 0; i < current_head_row; i++)
    {
        buffer[buffer_index] = fb->buffer[i];
        buffer_index++;
    }

    xSemaphoreGive(fb->mutex);
}

/**
 * @brief Tracks the joystick position.
 *
 * Updates the postion of the joystick.
 *
 * @param pvParameters A parameter used by FreeRTOS when calling with @see xTaskCreate.
 * @return None.
 */
void task_update_joystick(void *pvParameters)
{
    while (1)
    {
        joystick_loc = analogRead(A0) / ANALOG_READ_SCALE;
    }
}

/**
 * @brief Renders the game onto the LED matrix.
 *
 * Renders the game to the 8x8 LED matrix. All lines stored in the framebuffer are
 * rendered, as well as the current joystick position.
 *
 * @param pvParameters A parameter used by FreeRTOS when calling with @see xTaskCreate.
 * @return None.
 */
void task_render(void *pvParameters)
{
    pinMode(DIN, OUTPUT);
    pinMode(CS, OUTPUT);
    pinMode(CLK, OUTPUT);
    digitalWrite(CS, HIGH);
    spiTransfer(OP_DISPLAYTEST, 0);
    spiTransfer(OP_SCANLIMIT, 7);
    spiTransfer(OP_DECODEMODE, 0);
    spiTransfer(OP_SHUTDOWN, 1);

    byte current_frame[FRAME_BUFFER_LEN];
    fb_init(&fb);
    while (1)
    {
        fb_read(&fb, current_frame);
        for (byte i = 0; i < FRAME_BUFFER_LEN; i++)
        {
            spiTransfer(i, current_frame[i]);
        }
        spiTransfer(LED_MATRIX_LEN - 1, 1 << joystick_loc);
    }
}

/**
 * @brief Advances the game by one frame and determines when the game ends.
 *
 * Advances the game by writing a line of an LED pattern to the framebuffer.
 * The line will appear at the top of the screen and the line at the bottom
 * of the screen will be removed. If the joystick light collides with a light
 * on the last line as the frame is being advanced, the game will end.
 *
 * @param pvParameters A parameter used by FreeRTOS when calling with @see xTaskCreate.
 * @return None.
 */
void task_advance(void *pvParameters)
{
    byte current_frame[FRAME_BUFFER_LEN];
    randomSeed(analogRead(A2));
    while (1)
    {
        fb_read(&fb, current_frame);
        if (1 << joystick_loc & current_frame[FRAME_BUFFER_LEN - 1])
        {
            xQueueSend(score_signal, (void *)&dummy, 0);
            vTaskSuspend(NULL);
        }

        byte row = 0;
        for (int i = 0; i < LED_MATRIX_LEN; i++)
        {
            row <<= 1;
            if (random(LIGHT_CHANCE) == 0)
            {
                row |= 1;
            }
        }
        fb_write_line(&fb, row);
        vTaskDelay((unsigned int)mspt / portTICK_PERIOD_MS);
    }
}

/**
 * @brief Increases the difficulty(speed) of the game.
 *
 * The longer the game runs, the more difficult it will become.
 *
 * @param pvParameters A parameter used by FreeRTOS when calling with @see xTaskCreate.
 * @return None.
 */
void task_scale_difficulty(void *pvParameters)
{
    while (1)
    {
        vTaskDelay(DIFFICULTY_DELAY / portTICK_PERIOD_MS);
        mspt /= MSPT_SCALING;
    }
}

/**
 * @brief Displays the current score of the game on the LCD.
 *
 * Calculates and prints out the score of the current game onto the LCD.
 * Displays "Game Over" if the game is over.
 *
 * @param pvParameters A parameter used by FreeRTOS when calling with @see xTaskCreate.
 * @return None.
 */
void task_score(void *pvParameters)
{
    lcd.begin(16, 2);
    lcd.print("Score:");
    while (1)
    {
        if (xQueueReceive(score_signal, &dummy, 0))
        {
            lcd.setCursor(0, 0);
            lcd.print("Game Over");
            vTaskSuspend(NULL);
        }
        lcd.setCursor(0, 1);
        lcd.print(millis() / 100);
    }
}

/**
 * @brief Sets up the appropriate codes in the Arduino for the game.
 *
 * Creates tasks for the necessary functions for the game.
 *
 * @return None.
 */
void setup()
{
    Serial.begin(115200);
    while (!Serial)
    {
    }
    Serial.println();

    xTaskCreate(task_rt1, "rt1", 125, NULL, 3, NULL);
    xTaskCreate(task_update_joystick, "update_joystick", 1000, NULL, 3, NULL);
    xTaskCreate(task_render, "render", 1000, NULL, 3, NULL);
    xTaskCreate(task_advance, "advance", 1000, NULL, 3, NULL);
    xTaskCreate(task_scale_difficulty, "scale_difficulty", 1000, NULL, 3, NULL);
    xTaskCreate(task_score, "score", 1000, NULL, 3, NULL);
    vTaskStartScheduler();
}

void loop() {}

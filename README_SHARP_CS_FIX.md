# Sharp CS diagnostic fix

This build changes the LS027B7DH01 bring-up in three important ways:

1. GPIO14 (SCS/CS) is controlled manually as **active HIGH**. The previous
   version gave GPIO14 to the ESP-IDF hardware-CS engine, which drives CS
   active LOW by default. That is the opposite polarity required by this LCD.
2. SPI is reduced to 2 MHz for the first reliable test.
3. A 500 ms software VCOM toggle task is started after a successful refresh.

Expected result: a stable white screen with a black border, text and test boxes.
If there is still noise, capture GPIO14, GPIO12 and GPIO11 with a logic analyzer
or oscilloscope. GPIO14 must remain LOW idle and go HIGH for each complete SPI
transaction.

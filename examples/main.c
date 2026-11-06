#include "srl.h"

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

SRL_INT(g_lives, 1);
SRL_INT_RANGE(g_speed, 5, 1, 10);
SRL_FLOAT(g_height, 3.5);

SRL_STR(g_name, "NULL");

int main(void)
{
    while (true)
    {
        printf("lives: %d, speed: %d, height: %f\n", g_lives, g_speed, g_height);
        printf("my name is %s!\n", g_name);
        sleep(1);
    }
    return 0;
}
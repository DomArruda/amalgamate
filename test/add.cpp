#include "add.hpp"

int add(int x, int y)
{
    return x + y;
}

int add(Adder adder)
{
    return adder.x + adder.y;

}
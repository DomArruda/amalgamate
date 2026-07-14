#define ADDER

struct Adder
{
    int x{};
    int y{};

    Adder(int _x, int _y): x(_x), y(_y){}
    ~Adder() = default;

};
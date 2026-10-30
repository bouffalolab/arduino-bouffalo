// C++ library check: compile/link/run string, shared_ptr, vector, map, function, ostringstream and time().
// Print once per second; sum remains 28 ((7 + 7) * 2) and t increases.
#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

/**
 * @brief Initialize the resources used by this example or validation sketch.
 */
void setup()
{
    Serial.begin(115200);
}

/**
 * @brief Service the example or validation scenario on each Arduino loop iteration.
 */
void loop()
{
    auto values = std::make_shared<std::vector<int>>(3, 7);
    std::map<std::string, std::function<int(int)>> ops{{"x2", [](int v) { return v * 2; }}};
    std::ostringstream line;

    line << "uptime " << millis() << " sum=" << ops["x2"]((*values)[0] + (*values)[2])
         << " t=" << static_cast<long>(time(nullptr));
    Serial.println(line.str().c_str());
    delay(1000);
}

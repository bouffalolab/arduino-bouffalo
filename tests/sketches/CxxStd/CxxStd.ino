// C++ 标准库检查：string、shared_ptr、vector、map、function、ostringstream、time() 能编译链接并运行。
// 每秒输出一行，sum 恒为 28（(7 + 7) * 2），t 随时间递增
#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

void setup()
{
    Serial.begin(115200);
}

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

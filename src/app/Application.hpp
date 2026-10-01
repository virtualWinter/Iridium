#pragma once

#include <QApplication>

namespace iridium {

class Application final {
public:
    Application(int& argc, char** argv);
    int run();

private:
    QApplication m_application;
};

} // namespace iridium

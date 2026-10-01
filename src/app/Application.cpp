#include "app/Application.hpp"

#include "browser/Browser.hpp"

namespace iridium {

Application::Application(int& argc, char** argv)
    : m_application(argc, argv)
{
    m_application.setApplicationName("Iridium");
    m_application.setOrganizationName("Iridium");
}

int Application::run()
{
    const QStringList arguments = m_application.arguments();
    const std::string initialUrl = arguments.size() > 1
        ? arguments.at(1).toStdString() : "https://example.com";
    Browser browser(initialUrl);
    browser.window().show();
    return m_application.exec();
}

} // namespace iridium

#include "services.h"

Services* Services::m_instance = nullptr;

Services* Services::instance(QObject *parent)
{
    if (Services::m_instance == nullptr) {
        Services::m_instance = new Services(parent);
    }

    return Services::m_instance;
}

Services::Services(QObject* parent) :
    QObject(parent),
    renderingControl(new RenderingControl(parent)),
    avTransport(new AVTransport(parent))
{
}

#ifndef KIMG_PXL_H
#define KIMG_PXL_H

#include <QImageIOPlugin>

class PxlPlugin : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "pxl.json")

public:
    Capabilities capabilities(QIODevice* device, const QByteArray& format) const override;
    QImageIOHandler* create(QIODevice* device, const QByteArray& format = QByteArray()) const override;
};

#endif /* KIMG_PXL_H */

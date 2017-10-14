#include "patchmaker.h"

#include <QDir>
#include <QFile>
#include <QDebug>
#include <QStringList>
#include <QDebug>
#include <QMessageBox>
#include <QTextStream>

#include "Filesystem/filesystem.h"
#include "exheader.h"

static const QStringList requiredFiles =
{
    "Makefile", "loader/Makefile", "code.bin", "exheader.bin"
};


PatchMaker::PatchMaker(QObject* parent) :
    QObject(parent),
    m_pathValid(false)
{
    loaderCompiler = new PatchCompiler(this);
    compiler = new PatchCompiler(this);

    connect(loaderCompiler, SIGNAL(finished(int)), this, SLOT(loaderCompilerDone(int)));
    connect(compiler, SIGNAL(finished(int)), this, SLOT(compilerDone(int)));

    connect(loaderCompiler, SIGNAL(outputUpdate(QString)), this, SLOT(onLoaderCompilerOutput(QString)));
    connect(compiler, SIGNAL(outputUpdate(QString)), this, SLOT(onCompilerOutput(QString)));

    emit setBusy(false);
}

PatchMaker::~PatchMaker()
{

}

bool PatchMaker::setPath(const QString& newPath)
{
    QStringList missingFiles;
    for (int i = 0; i < requiredFiles.count(); i++)
    {
        if (!QFile(newPath + "/" + requiredFiles.at(i)).exists())
            missingFiles.append("/" + requiredFiles.at(i));
    }

    if (missingFiles.count() > 0)
    {
        QString missingFilesStr = "The working directory in invalid. The following files are missing:\n";
        for (int i = 0; i < missingFiles.count(); i++)
            missingFilesStr += "\n - " + missingFiles.at(i);

        QMessageBox::information(NULL, "Magikoopa", missingFilesStr, QMessageBox::Ok);
        return false;
    }


    m_path = newPath;
    m_pathValid = true;

    loaderCompiler->setPath(m_path + "/loader");
    compiler->setPath(m_path);

    checkBackup();

    Exheader exHeader(new ExternalFile(NULL, m_path + "/bak/exheader.bin"));
    m_loaderOffset = (exHeader.data.sci.textCodeSetInfo.size + 0x100000 + 0xF) & ~0xF;
    m_loaderMaxSize = exHeader.data.sci.readOnlyCodeSetInfo.address - m_loaderOffset;
    m_newCodeOffset = exHeader.data.sci.dataCodeSetInfo.address + (exHeader.data.sci.dataCodeSetInfo.physicalRegionSize << 12) + ((exHeader.data.sci.bssSize + 0xFFF) & ~0xFFF);

    emit addOutput("Info", QString("Game Name:           %1").arg(exHeader.data.sci.title), false);
    emit addOutput("Info", QString("Loader Offset:       %1").arg(m_loaderOffset, 8, 0x10, QChar('0')), false);
    emit addOutput("Info", QString("Loader maximum Size: %1").arg(m_loaderMaxSize, 8, 0x10, QChar('0')), false);
    emit addOutput("Info", QString("New Code Offset:     %1").arg(m_newCodeOffset, 8, 0x10, QChar('0')), false);

    emit updateStatus("Ready");

    return true;
}

void PatchMaker::makeInsert()
{
    emit setBusy(true);
    restoreFromBackup();
    emit updateStatus("Running Make...");
    compiler->make(m_newCodeOffset);
}

void PatchMaker::makeClean()
{
    emit updateStatus("Making Clean...");
    emit setBusy(true);
    compiler->clean();
}

void PatchMaker::loaderCompilerDone(int exitCode)
{
    if (loaderCompiler->lastAction() == PatchCompiler::CompilerAction_Clean)
    {
        emit updateStatus("Clean");
        emit setBusy(false);
    }

    else if (loaderCompiler->lastAction() == PatchCompiler::CompilerAction_Make)
    {
        if (exitCode == 0) insert();
        else
        {
            emit updateStatus("Compilation Failed (Loader)");
            emit setBusy(false);
        }
    }
}

void PatchMaker::compilerDone(int exitCode)
{
    if (compiler->lastAction() == PatchCompiler::CompilerAction_Clean)
        loaderCompiler->clean();

    else if (compiler->lastAction() == PatchCompiler::CompilerAction_Make)
    {
        if (exitCode == 0)
        {
            emit updateStatus("Running Make (Loader)...");

            QFile codesizeFile(m_path + "/loader/source/codesize.h");
            codesizeFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
            QTextStream out(&codesizeFile);
            out << "#ifndef CODESIZE\n"
                << QString("#define CODESIZE 0x%1\n").arg(QFile(m_path + "/newcode.bin").size(), 8, 0x10, QChar('0'));
            codesizeFile.close();

            QFile newcodeFile(m_path + "/newcode.bin");
            m_loaderDataOffset = m_newCodeOffset + ((newcodeFile.size() + 0xF) & ~0xF);

            loaderCompiler->make(m_loaderOffset, m_loaderDataOffset);
        }
        else
        {
            emit updateStatus("Compilation Failed (Loader)");
            emit setBusy(false);
        }
    }
}

void checkListForSymVar(quint32* var, bool* foundVar, QStringList* segs, const QString& name)
{
    if (*foundVar)
        return;

    if (segs->at(segs->count()-1) == name)
    {
        bool ok;
        *var = segs->at(0).toUInt(&ok, 0x10);
        if (!ok) return;
        else
        {
            *foundVar = true;
            return;
        }
    }
}

void PatchMaker::insert()
{
    emit updateStatus("Inserting...");

    // Get LoaderMain address + __text_end
    QFile loaderSymFile(m_path + "/loader/loader.sym");
    loaderSymFile.open(QIODevice::ReadOnly | QIODevice::Text);
    QTextStream loaderSym(&loaderSymFile);

    quint32 loaderMainAddr;
    quint32 loaderTextEnd;
    quint32 loaderDataStart;
    quint32 loaderDataEnd;

    bool loaderMainAddrFound = false;
    bool loaderTextEndFound = false;
    bool loaderDataStartFound = false;
    bool loaderDataEndFound = false;

    while (!loaderSym.atEnd())
    {
        QString line = loaderSym.readLine();
        QStringList segs = line.split(" ");
        if (segs.count() < 2)
            continue;

        checkListForSymVar(&loaderMainAddr, &loaderMainAddrFound, &segs, "LoaderMain");
        checkListForSymVar(&loaderTextEnd, &loaderTextEndFound, &segs, "__text_end");
        checkListForSymVar(&loaderDataStart, &loaderDataStartFound, &segs, "__data_start");
        checkListForSymVar(&loaderDataEnd, &loaderDataEndFound, &segs, "__data_end");
    }
    loaderSymFile.close();

    if (!loaderMainAddrFound)
    {
        emit updateStatus("LoaderMain not found");
        emit setBusy(false);
        return;
    }
    else if (!loaderTextEndFound || !loaderDataStartFound || !loaderDataEndFound)
    {
        emit updateStatus("Parsing Loader sections failed");
        emit setBusy(false);
        return;
    }


    ExternalFile* codeFile = new ExternalFile(m_path + "/code.bin");
    ExternalFile* loaderFile = new ExternalFile(m_path + "/loader/loader.bin");
    ExternalFile* newCodeFile = new ExternalFile(m_path + "/newcode.bin");

    codeFile->open();
    loaderFile->open();
    newCodeFile->open();

    quint32 oldCodeSize = codeFile->size();
    codeFile->resize(loaderDataEnd - 0x100000);
    codeFile->seek(oldCodeSize);

    // Clear BSS section
    while (codeFile->pos() < m_newCodeOffset - 0x100000)
        codeFile->write8(0);

    // Insert Loader Text
    quint32 loaderTextSize = loaderTextEnd - m_loaderOffset;
    quint8* loaderText = new quint8[loaderTextSize];
    loaderFile->seek(0);
    loaderFile->readData(loaderText, loaderTextSize);
    codeFile->seek(m_loaderOffset - 0x100000);
    codeFile->writeData(loaderText, loaderTextSize);
    delete loaderText;

    // Clear Padding to Loader Data
    while (codeFile->pos() < loaderDataStart - 0x100000)
        codeFile->write8(0);

    // Insert Loader Data
    quint32 loaderDataSize = loaderDataEnd - loaderDataStart;
    quint8* loaderData = new quint8[loaderDataSize];
    loaderFile->seek(loaderDataStart - m_loaderOffset);
    loaderFile->readData(loaderData, loaderDataSize);
    codeFile->seek(loaderDataStart - 0x100000);
    qDebug() << QString::number(loaderDataStart, 0x10);
    qDebug() << QString::number(m_loaderOffset, 0x10);
    qDebug() << QString::number(loaderDataStart - m_loaderOffset, 0x10);
    codeFile->writeData(loaderData, loaderDataSize);

    // Make a nice hook? to loader
    //quint32 asdf = makeBranchOpcode(0x00100000, loaderMainAddr, true);
    //codeFile->seek(0x00100000 - 0x100000);
    //codeFile->write32(asdf);

    // Insert New Code

    // Do hooks 'n stuff

    codeFile->save();
    codeFile->close();

    loaderFile->close();
    newCodeFile->close();

    delete codeFile;
    delete loaderFile;
    delete newCodeFile;

    emit updateStatus("Fixing Exheader");
    fixExheader(loaderDataEnd - m_newCodeOffset);
}

void PatchMaker::fixExheader(quint32 newCodeSize)
{
    Exheader exHeader(new ExternalFile(m_path + "/exheader.bin"));

    exHeader.data.sci.textCodeSetInfo.size = exHeader.data.sci.textCodeSetInfo.physicalRegionSize << 12;

    exHeader.data.sci.dataCodeSetInfo.physicalRegionSize += ((exHeader.data.sci.bssSize + 0xFFF) & ~0xFFF) >> 12 ;
    exHeader.data.sci.dataCodeSetInfo.physicalRegionSize += ((newCodeSize + 0xFFF) & ~0xFFF) >> 12;
    exHeader.data.sci.dataCodeSetInfo.size = exHeader.data.sci.dataCodeSetInfo.physicalRegionSize << 12;

    exHeader.data.sci.bssSize = 0;

    exHeader.save();

    emit setBusy(false);
    emit updateStatus("All done");
}

quint32 PatchMaker::makeBranchOpcode(quint32 src, quint32 dest, bool link)
{
    quint32 ret = 0xEA000000;
    if (link) ret |= 0x01000000;

    int offset = (dest / 4) - (src / 4) - 2;
    offset &= 0x00FFFFFF;

    ret |= offset;

    return ret;
}

void PatchMaker::checkBackup()
{
    QDir(m_path).mkdir("bak");

    if (!QFile(m_path + "/bak/code.bin").exists())
        QFile(m_path + "/code.bin").copy(m_path + "/bak/code.bin");

    if (!QFile(m_path + "/bak/exheader.bin").exists())
        QFile(m_path + "/exheader.bin").copy(m_path + "/bak/exheader.bin");
}

void PatchMaker::restoreFromBackup()
{
    QDir dir(m_path);

    if (QFile(m_path + "/bak/code.bin").exists())
    {
        dir.remove("code.bin");
        QFile(m_path + "/bak/code.bin").copy(m_path + "/code.bin");
    }

    if (QFile(m_path + "/bak/exheader.bin").exists())
    {
        dir.remove("exheader.bin");
        QFile(m_path + "/bak/exheader.bin").copy(m_path + "/exheader.bin");
    }
}

void PatchMaker::onLoaderCompilerOutput(const QString& text)
{
    emit addOutput("Loader", text, false);
}

void PatchMaker::onCompilerOutput(const QString& text)
{
    emit addOutput("Compiler", text, false);
}
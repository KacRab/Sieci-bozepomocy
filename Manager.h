#pragma once
#include <QFile>
#include <QMessageBox>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QWidget>
#include "GenWartZadana.h"
#include "SprzezenieZwrotne.h"
#include <cassert>
#include <vector>

class Manager : public QObject
{
    Q_OBJECT

public:
    Manager()
        : QObject()
        , server(nullptr) // Musi być najpierw
        , serverSocket(nullptr)
        , clientSocket(nullptr)
        , gen_wart() // Następnie inicjalizacja gen_wart
        , sprzezeniezwrotne()
        , dataReceived(false)
    {
        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &Manager::wykonajTakt);
    }
    ~Manager() {}

    void startTaktowanie()
    {
        timer->start(1000); // Takt co 1000 ms (możesz dostosować interwał)
        emit statusChanged("Taktowanie jednostronne rozpoczęte.");
    }

    void stopTaktowanie()
    {
        timer->stop();
        emit statusChanged("Taktowanie jednostronne zatrzymane.");
    }

    void stopServer()
    {
        if (server) {
            server->close();
            serverSocket = nullptr;
            delete server;
            server = nullptr;
            emit statusChanged("Serwer zatrzymany.");
        }
    }

    void disconnectClient()
    {
        if (clientSocket) {
            clientSocket->disconnectFromHost();
            delete clientSocket;
            clientSocket = nullptr;
            emit statusChanged("Rozłączono z serwerem.");
        }
    }

    void sendMessage(const QString &message)
    {
        if (serverSocket) {
            serverSocket->write(message.toUtf8());
        } else if (clientSocket) {
            clientSocket->write(message.toUtf8());
        }
    }

    void sendParameters()
    {
        if (clientSocket && clientSocket->isOpen()) {
            QString parametry = "PID=1.0,0.1,0.01;ARX=1.0,0.5,0.1";
            clientSocket->write(parametry.toUtf8());
            qDebug() << "Wysłano parametry: " << parametry;
        }
    }

    void receiveParameters(const QString &parametry)
    {
        QStringList parts = parametry.split(";");
        QStringList pidParams = parts[0].split(",");
        QStringList arxParams = parts[1].split(",");

        // Ustaw PID
        std::vector<double> parametryPID = {pidParams[0].toDouble(),
                                            pidParams[1].toDouble(),
                                            pidParams[2].toDouble()};
        sprzezeniezwrotne.setPID(parametryPID);

        // Ustaw ARX
        std::vector<double> A = {arxParams[0].toDouble()};
        std::vector<double> B = {arxParams[1].toDouble()};
        int delay = arxParams[2].toInt();
        sprzezeniezwrotne.setARX(A, B, delay);
        qDebug() << "Odebrano i ustawiono parametry: " << parametry;
    }

    void startServer(int port)
    {
        if (server)
            return; // Unikaj wielokrotnego uruchamiania serwera

        server = new QTcpServer(this);

        // Obsługa połączeń od regulatora (klienta)
        connect(server, &QTcpServer::newConnection, this, [this]() {
            serverSocket = server->nextPendingConnection();

          //  connect(serverSocket, &QTcpSocket::readyRead, this, &Manager::receiveSignal);
           // qDebug() << "Połączono sygnał readyRead dla klienta.";



            // Odbieranie sygnału sterującego od regulatora
            connect(serverSocket, &QTcpSocket::readyRead, this, [this]() {
                QByteArray data = serverSocket->readAll();
                qDebug() << "Data: " << data.toStdString();
                double sygnalSterujacy = QString(data).toDouble(); // Odebrany sygnał sterujący
                wyniki[5] = sygnalSterujacy;
                qDebug() << "Otrzymano sygnał sterujący: " << sygnalSterujacy;

                // Oblicz wartość regulowaną (ARX)
                double wartoscRegulowana = wyniki[6];//sprzezeniezwrotne.SimUAR(sygnalSterujacy)[6];

                // Wyślij wartość regulowaną do regulatora
                serverSocket->write(QString::number(wartoscRegulowana).toUtf8());
                qDebug() << "Wysłano wartość regulowaną: " << wartoscRegulowana;
                dataReceived = true;

            });

            emit statusChanged("Połączono z regulatorem (klientem)!");
        });

        if (!server->listen(QHostAddress::Any, port)) {
            emit statusChanged("Nie udało się uruchomić serwera!");
        } else {
            emit statusChanged(QString("Serwer działa na porcie %1").arg(port));
        }
    }

    void connectToServer(const QString &address, int port)
    {
        if (clientSocket)
            return; // Unikaj wielokrotnego tworzenia socketu

        clientSocket = new QTcpSocket(this);

       // connect(clientSocket, &QTcpSocket::readyRead, this, &Manager::receiveSignal);


        // Połączenie z serwerem (obiekt regulacji)
        connect(clientSocket, &QTcpSocket::readyRead, this, [this]() {
            QByteArray data = clientSocket->readAll();
            double wartoscRegulowana = QString(data).toDouble(); // Odebrana wartość regulowana
            wyniki[6] = wartoscRegulowana;
            sprzezeniezwrotne.setRegulowana(wyniki);
            qDebug() << "Otrzymano wartość regulowaną: " << wartoscRegulowana;

            // Oblicz sygnał sterujący (PID)
            //double wartZadana = gen_wart.GenerujSygnal(timer->interval());
            //std::vector<double> wynikiPID = sprzezeniezwrotne.SimUAR(wartZadana);
            double sygnalSterujacy = wyniki[5];

            // Wyślij sygnał sterujący do obiektu regulacji
            clientSocket->write(QString::number(sygnalSterujacy).toUtf8());
            qDebug() << "Wysłano sygnał sterujący: " << sygnalSterujacy;
            dataReceived = true;

        });

        connect(clientSocket, &QTcpSocket::connected, this, [=]() {
            emit statusChanged("Połączono z serwerem!");
        });

        connect(clientSocket, &QTcpSocket::disconnected, this, [=]() {
            emit statusChanged("Rozłączono z serwerem.");
        });

        clientSocket->connectToHost(address, port);

        if (!clientSocket->waitForConnected(3000)) {
            emit statusChanged("Nie udało się połączyć z serwerem!");
        }
    }

    void switchMode(bool isNetworkMode)
    {
        emit modeChanged(isNetworkMode); // Emituj wartość true (sieć) lub false (offline)
        qDebug() << "Tryb pracy: " << (isNetworkMode ? "Online" : "Offline");
    }

signals:
    void statusChanged(const QString &status); // Informacja o stanie połączenia
    void modeChanged(bool isRegulator);        // Zmiana trybu pracy
    void enableButtons(bool enable);

public slots:
    void wykonajTakt()
    {
        // dataReceived = true;
        qInfo() << __FUNCTION__;
        // Wyślij sygnał sterujący do obiektu
        //double wartZadana = gen_wart.GenerujSygnal(timer->interval());
        // sendMessage(QString::number(wartZadana));
        sendMessage("Testowa wiadomość z klienta");

        // Sprawdź, czy odpowiedź dotarła
        if (dataReceived) {
            // Procesuj otrzymane dane
            processResponseData();
            emit statusChanged("Symulacja wyrabia");
            qDebug() << "Emit: Symulacja wyrabia";

        } else {
            // Użyj poprzednich danych
            usePreviousData();
            emit statusChanged("Symulacja nie wyrabia");
            qDebug() << "Emit: Symulacja nie wyrabia";
        }

        // Reset flagi na kolejny takt
        dataReceived = false;
    }

    void setGenerator(Sygnal typ, std::vector<double> &ParametryGen)
    {
        gen_wart.setGen(typ, ParametryGen);
    }
    void setRegulatorPID(std::vector<double> &ParametryPID)
    {
        sprzezeniezwrotne.setPID(ParametryPID);
    }
    void setTrybPID(const bool Tryb) { sprzezeniezwrotne.setTrybPID(Tryb); }
    void setModelARX(std::vector<double> &A, std::vector<double> &B, int delay, double Z = 0.0)
    {
        sprzezeniezwrotne.setARX(A, B, delay, Z);
    }
    void ResetSim() { sprzezeniezwrotne.ResetSim(); }
    void ResetPID() { sprzezeniezwrotne.ResetPID(); }

    std::vector<double> Symuluj(double czas)
    {
        double wartZadana = gen_wart.GenerujSygnal(czas);
        if(clientSocket){
            wyniki = sprzezeniezwrotne.SimPIDOnline(wartZadana);
            return wyniki;
        }else if(server){
            wyniki[6]= sprzezeniezwrotne.zwrocY(wyniki)[6];
            return wyniki;
        }else{
            return sprzezeniezwrotne.SimUAR(wartZadana);}
    }

    void zapisz(const std::vector<double> &ParametryPID,
                const std::vector<double> &A,
                const std::vector<double> &B,
                const int delay,
                const double zaklucenia,
                const std::vector<double> &ParametryGen)
    {
        QString nazwaPliku = "Dane.txt";
        QFile plik(nazwaPliku);

        if (!plik.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qDebug() << "Nie udało się otworzyć pliku!";
            return;
        }

        QTextStream out(&plik);

        // Zapis PID:
        zapiszWiersz(out, ParametryPID);
        out << "\n";

        // Zapis ARX:
        zapiszWiersz(out, A);
        out << "|";
        zapiszWiersz(out, B);
        out << "|" << delay << "|" << zaklucenia << "\n";

        // Zapis Generatora:
        zapiszWiersz(out, ParametryGen);

        plik.close();
    }

    void odczyt(std::vector<double> &ParametryPID,
                std::vector<double> &A,
                std::vector<double> &B,
                int &delay,
                double &zaklucenia,
                std::vector<double> &ParametryGen)
    {
        QString nazwaPliku = "Dane.txt";
        QFile plik(nazwaPliku);

        if (!plik.exists()) {
            QMessageBox::critical(nullptr, "Błąd!", "Plik nie istnieje!");
            return;
        }

        plik.open(QIODevice::ReadOnly | QIODevice::Text);
        QTextStream in(&plik);

        // Odczyt PID:
        odczytajWiersz(in, ParametryPID);

        // Odczyt ARX (A | B | delay | zaklucenia):
        QString line;
        in.readLineInto(&line);
        QStringList parts = line.split("|");

        odczytajWiersz(parts[0], A);
        odczytajWiersz(parts[1], B);

        delay = parts[2].toInt();
        zaklucenia = parts[3].toDouble();

        // Odczyt Generatora:
        odczytajWiersz(in, ParametryGen);

        plik.close();
    }

private slots:
    void receiveSignal()
    {
        QByteArray data;
        if (clientSocket) {
            data = clientSocket->readAll();
            qDebug() << "Klient odebrał dane: " << QString(data);
            // Przetwarzanie danych
            double wynik = 5;
            dataReceived = true;

            // Wysłanie odpowiedzi zwrotnej do serwera
            clientSocket->write(QString::number(wynik).toUtf8());
            qDebug() << "Klient wysłał odpowiedź: " << wynik;

        } else if (serverSocket) {
            data = serverSocket->readAll();
            qDebug() << "Serwer odebrał odpowiedź: " << QString(data);
            // Ustawienie flagi wskazującej, że dane dotarły
            dataReceived = true;
            qDebug() << "Flaga dataReceived ustawiona na true.";
            // Wysłanie odpowiedzi zwrotnej do klienta
            serverSocket->write("Odpowiedź od serwera: Dane przetworzone");
            qDebug() << "Serwer wysłał odpowiedź zwrotną do klienta.";
        }
        // qInfo() << "Received data: " << data;
        // dataReceived = true; // Dane dotarły
        // emit statusChanged(QString("Odebrano wiadomość: %1").arg(QString(data)));
    }

private:
    QTcpServer *server;       // Obiekt serwera TCP
    QTcpSocket *serverSocket; // Gniazdo połączenia serwera
    QTcpSocket *clientSocket;
    SprzezenieZwrotne sprzezeniezwrotne;
    GenWartZadana gen_wart;
    QTimer *timer;     // Timer do taktowania
    bool dataReceived; // Flaga czy dane dotarły od obiektu
    std::vector <double> wyniki = {0.0,0.0,0.0,0.0,0.0,0.0};

    void processResponseData()
    {
        qInfo() << "Przetwarzanie odebranych danych...";
        // Tu dodaj logikę przetwarzania danych
    }

    void usePreviousData()
    {
        qInfo() << "Brak danych - użycie poprzedniej wartości.";
        // Tu dodaj logikę korzystania z poprzedniej wartości
    }

    void zapiszWiersz(QTextStream &out, const std::vector<double> &vec)
    {
        for (size_t i = 0; i < vec.size(); ++i) {
            out << vec[i];
            if (i < vec.size() - 1) {
                out << ",";
            }
        }
    }
    void odczytajWiersz(QTextStream &in, std::vector<double> &vec)
    {
        QString line;
        in.readLineInto(&line);
        odczytajWiersz(line, vec);
    }

    void odczytajWiersz(const QString &line, std::vector<double> &vec)
    {
        vec.clear();
        QStringList numbers = line.split(",");
        for (const QString &num : numbers) {
            vec.push_back(num.toDouble());
        }
    }
};

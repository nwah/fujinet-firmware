#ifndef NEONMAKE_H
#define NEONMAKE_H

#include "../network-protocol/Protocol.h"

class NeonMake
{
public:
    NeonMake();
    virtual ~NeonMake();

    void setProtocol(NetworkProtocol *newProtocol);
    bool status(NetworkStatus *status);
    
    bool parse();
    int readDocLen();
    bool readDoc(uint8_t *rx_buf, unsigned short len);
    int neon_bytes_remaining = 0;

private:
    NetworkProtocol *_protocol = nullptr;
    string _parseBuffer;
    string _doc;

    string compile(string adf);
};

#endif // NEONMAKE_H
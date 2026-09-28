#include "NukiRetryHandler.h"
#include "Logger.h"
#include "../ProtectWebhook.h"

NukiRetryHandler::NukiRetryHandler(std::string reference, Gpio* gpio, std::vector<uint8_t> pinsComm, std::vector<uint8_t> pinsCommError, int nrOfRetries, int retryDelay)
: _reference(reference),
  _gpio(gpio),
  _pinsComm(pinsComm),
  _pinsCommError(pinsCommError),
  _nrOfRetries(nrOfRetries),
  _retryDelay(retryDelay)
{
}

const Nuki::CmdResult NukiRetryHandler::retryComm(std::function<Nuki::CmdResult()> func)
{
    Nuki::CmdResult cmdResult = Nuki::CmdResult::Error;

    int retryCount = 0;

    setCommPins(HIGH);
#ifdef NUKI_HUB_PROTECT_WEBHOOK
    _stopRetrying = false;
#endif

    while(retryCount < _nrOfRetries + 1 && cmdResult != Nuki::CmdResult::Success)
    {
        wdtReset();
#ifdef NUKI_HUB_PROTECT_WEBHOOK
        ProtectWebhook::bleHeartbeat();
#endif

        cmdResult = func();

        if (cmdResult != Nuki::CmdResult::Success)
        {
            setCommErrorPins(HIGH);
            ++retryCount;

#ifdef NUKI_HUB_PROTECT_WEBHOOK
            // Fork: the log says what really happens next. Upstream printed
            // "Retry 4 of 3" after the last attempt, and "retrying" lines after
            // the caller had decided not to send again.
            if(_stopRetrying)
            {
                break; // the caller logged why; nothing more is sent
            }
            if(retryCount > _nrOfRetries)
            {
                Log->print(_reference.c_str());
                Log->print(": Last command failed, no retries left (");
                Log->print(_nrOfRetries);
                Log->println(_nrOfRetries == 1 ? " retry done)" : " retries done)");
            }
            else
#endif
            {
                Log->print(_reference.c_str());
                Log->print(": Last command failed, retrying after ");
                Log->print(_retryDelay);
                Log->print(" milliseconds. Retry ");
                Log->print(retryCount);
                Log->print(" of ");
                Log->println(_nrOfRetries);
            }

            espDelayAck(_retryDelay);
        }
    }
    setCommPins(LOW);
    setCommErrorPins(LOW);

    return cmdResult;
}

void NukiRetryHandler::setCommPins(const uint8_t& value)
{
    _gpio->setPinOutput(_pinsComm, value);
}

void NukiRetryHandler::setCommErrorPins(const uint8_t& value)
{
    _gpio->setPinOutput(_pinsCommError, value);
}



/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI
    Copyright (C) 2016 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifdef _WIN32
#define NOMINMAX
#endif

#include "DeviceThread.h"
#include "DeviceEditor.h"

#include "ImpedanceMeter.h"
#include "Headstage.h"

using namespace RhythmNode;

BoardType DeviceThread::boardType = ACQUISITION_BOARD; // initialize static member

#if defined(_WIN32)
#define okLIB_NAME "okFrontPanel.dll"
#define okLIB_EXTENSION "*.dll"
#elif defined(__APPLE__)
#define okLIB_NAME "libokFrontPanel.dylib"
#define okLIB_EXTENSION "*.dylib"
#elif defined(__linux__)
#define okLIB_NAME "./libokFrontPanel.so"
#define okLIB_EXTENSION "*.so"
#endif

//#define DEBUG_EMULATE_HEADSTAGES 8
//#define DEBUG_EMULATE_64CH

#define INIT_STEP ( evalBoard->isUSB3() ? 256 : 60)

DataThread* DeviceThread::createDataThread(SourceNode *sn)
{
    return new DeviceThread(sn, boardType);
}

DeviceThread::DeviceThread(SourceNode* sn, BoardType boardType_) : DataThread(sn),
    chipRegisters(30000.0f),
    deviceFound(false),
    isTransmitting(false),
    channelNamingScheme(GLOBAL_INDEX),
    updateSettingsDuringAcquisition(false)
{

    boardType = boardType_;

    impedanceThread = new ImpedanceMeter(this);

    memset(auxBuffer, 0, sizeof(auxBuffer));
    memset(auxSamples, 0, sizeof(auxSamples));

    for (int i = 0; i < 8; i++)
        adcRangeSettings[i] = 0;

    int maxNumHeadstages = (boardType == RHD_RECORDING_CONTROLLER) ? 16 : 8;

    for (int i = 0; i < maxNumHeadstages; i++)
        headstages.add(new Headstage(static_cast<Rhd2000EvalBoard::BoardDataSource>(i), maxNumHeadstages));

    evalBoard = new Rhd2000EvalBoard;

    sourceBuffers.add(new DataBuffer(2, 10000)); // start with 2 channels and automatically resize

    // Open Opal Kelly XEM6010 board.
    // Returns 1 if successful, -1 if FrontPanel cannot be loaded, and -2 if XEM6010 can't be found.

#if defined(__APPLE__)
    File appBundle = File::getSpecialLocation(File::currentApplicationFile);
    const String executableDirectory = appBundle.getChildFile("Contents/Resources").getFullPathName();
#else
    File executable = File::getSpecialLocation(File::currentExecutableFile);
    const String executableDirectory = executable.getParentDirectory().getFullPathName();
#endif

    String dirName = executableDirectory;
    libraryFilePath = dirName;
    libraryFilePath += File::getSeparatorString();
    libraryFilePath += okLIB_NAME;

    dacStream = new int[8];
    dacChannels = new int[8];
    dacThresholds = new float[8];
    dacChannelsToUpdate = new bool[8];

    if (openBoard(libraryFilePath))
    {
        dataBlock = new Rhd2000DataBlock(1, evalBoard->isUSB3());

        // upload bitfile and restore default settings
        initializeBoard();

        if (evalBoard->isUSB3())
            LOGD("USB3 board mode enabled");

        MAX_NUM_DATA_STREAMS = evalBoard->MAX_NUM_DATA_STREAMS;
        MAX_NUM_HEADSTAGES = MAX_NUM_DATA_STREAMS / 2;

        //std::cout << "MAX NUM STREAMS: " << MAX_NUM_DATA_STREAMS << ", MAX NUM HEADSTAGES: " << MAX_NUM_HEADSTAGES << std::endl;

        // automatically find connected headstages
        scanPorts(); // things would appear to run more smoothly if this were done after the editor has been created

        for (int k = 0; k < 8; k++)
        {
            dacChannelsToUpdate[k] = true;
            dacStream[k] = 0;
            setDACthreshold(k, 65534);
            dacChannels[k] = 0;
            dacThresholds[k] = 0;
        }
    }
}

DeviceThread::~DeviceThread()
{
    LOGD( "RHD2000 interface destroyed." );

    if (deviceFound && boardType == ACQUISITION_BOARD)
    {
        int ledArray[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        evalBoard->setLedDisplay(ledArray);
    }

    if (deviceFound)
        evalBoard->resetFpga();

    delete[] dacStream;
    delete[] dacChannels;
    delete[] dacThresholds;
    delete[] dacChannelsToUpdate;
}

void DeviceThread::initialize(bool signalChainIsLoading)
{
    if (signalChainIsLoading)
        return;

    // Let's turn one LED on to indicate that the board is now connected
    if (boardType == ACQUISITION_BOARD && deviceFound)
    {
        int ledArray[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
        evalBoard->setLedDisplay(ledArray);
    }
}

std::unique_ptr<GenericEditor> DeviceThread::createEditor(SourceNode* sn)
{

    std::unique_ptr<DeviceEditor> editor = std::make_unique<DeviceEditor>(sn, this);

    return editor;
}

void DeviceThread::handleBroadcastMessage(String msg)
{
    StringArray parts = StringArray::fromTokens(msg, " ", "");
    //LOGC("[dspw] bcast msg = ",msg);
    //std::cout << "Received " << msg << std::endl;

    if (parts[0].equalsIgnoreCase("ACQBOARD"))
    {
        if (parts.size() > 1)
        {
            String command = parts[1];

            if (command.equalsIgnoreCase("TRIGGER"))
            {
                if (parts.size() == 4)
                {
                    int ttlLine = parts[2].getIntValue() - 1;

                    if (ttlLine < 0 || ttlLine > 7)
                        return;

                    int eventDurationMs = parts[3].getIntValue();

                    if (eventDurationMs < 10 || eventDurationMs > 5000)
                        return;

                    DigitalOutputCommand command;
                    command.ttlLine = ttlLine;
                    command.state = true;
                    
                    digitalOutputCommands.push(command);

                    DigitalOutputTimer* timer = new DigitalOutputTimer(this, ttlLine, eventDurationMs);

                    digitalOutputTimers.add(timer);

                }
            }
        }
    }
    // added to handle DSPW messages
    if (parts[0].equalsIgnoreCase ("DSPW"))
    {
        int timerEventCmd = parts[3].getIntValue();
        
        // each eventstate bit corresponds to an event 0x1 = event 1, 0xf = event 1,2,3,4
        int eventAdjust[8] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80};
        int ttlLineAdjust = eventAdjust[timerEventCmd - 1];
        
        if (parts[1].equalsIgnoreCase ("RCB"))
        {
            if (parts.size() > 2)
            {
                String command = parts[2];
                
                if (command.equalsIgnoreCase ("TRIGGER"))  //Trigger vs Timer
                {
                    ttlLineAdjust = eventAdjust[timerEventCmd - 1];
                    // Set(1) or Clear(0) Event #
                    if (parts.size() == 5)
                    {
                        int ttlLine = parts[3].getIntValue(); // - 1;
                        if (ttlLine < 1 || ttlLine > 8)
                            //    if (ttlLine < 0 || ttlLine > 7)
                            return;
                        
                        int eventNum = parts[4].getIntValue();
                        if (eventNum < 0 || eventNum > 1)
                            return;
                        
                        if (eventNum == 1)
                        {
                            eventState =  ttlLineAdjust;
                        }
                        else if (eventNum == 0)
                        {
                            eventState = 0;
                        }
                    }
                }
            }
        }
    }

}


void DeviceThread::addDigitalOutputCommand(DigitalOutputTimer* timerToDelete, int ttlLine, bool state)
{
    DigitalOutputCommand command;
    command.ttlLine = ttlLine;
    command.state = state;

    digitalOutputCommands.push(command);

    digitalOutputTimers.removeObject(timerToDelete);
}

DeviceThread::DigitalOutputTimer::DigitalOutputTimer(DeviceThread* board_, int ttlLine_, int eventDurationMs)
    : board(board_)
{
    LOGC("[dspw] eventDurationMS = ",eventDurationMs);
    LOGC("[dspw] ttlLine = ",ttlLine_);
    
    tllOutputLine = ttlLine_;
    
    startTimer(eventDurationMs);
}

void DeviceThread::DigitalOutputTimer::timerCallback()
{
    stopTimer();

    board->addDigitalOutputCommand(this, tllOutputLine, false);
}

void DeviceThread::setDACthreshold(int dacOutput, float threshold)
{
    dacThresholds[dacOutput]= threshold;
    dacChannelsToUpdate[dacOutput] = true;
    updateSettingsDuringAcquisition = true;

    //evalBoard->setDacThresholdVoltage(dacOutput,threshold);
}

void DeviceThread::setDACchannel(int dacOutput, int channel)
{
    if (channel < getNumDataOutputs(ContinuousChannel::ELECTRODE))
    {
        int channelCount = 0;
        for (int i = 0; i < enabledStreams.size(); i++)
        {
            if (channel < channelCount + numChannelsPerDataStream[i])
            {
                dacChannels[dacOutput] = channel - channelCount;
                dacStream[dacOutput] = i;
                break;
            }
            else
            {
                channelCount += numChannelsPerDataStream[i];
            }
        }
        dacChannelsToUpdate[dacOutput] = true;
        updateSettingsDuringAcquisition = true;
    }
}

Array<int> DeviceThread::getDACchannels() const
{
    Array<int> dacChannelsArray;

    for (int k = 0; k < 8; ++k)
    {
        dacChannelsArray.add (dacChannels[k]);
    }

    return dacChannelsArray;
}


bool DeviceThread::openBoard(String pathToLibrary)
{
    int return_code = evalBoard->open(pathToLibrary.getCharPointer());

    if (return_code == 1)
    {
        deviceFound = true;
    }
    else if (return_code == -1) // dynamic library not found
    {
        bool response = AlertWindow::showOkCancelBox(AlertWindow::NoIcon,
                                                     "Opal Kelly library not found.",
                                                     "The Opal Kelly library file was not found in the directory of the executable. "
                                                     "Would you like to browse for it?",
                                                     "Yes", "No", 0, 0);
        if (response)
        {
            // browse for file
            FileChooser fc("Select the library file...",
                           File::getCurrentWorkingDirectory(),
                           okLIB_EXTENSION,
                           true);

            if (fc.browseForFileToOpen())
            {
                File currentFile = fc.getResult();
                libraryFilePath = currentFile.getFullPathName();
                openBoard(libraryFilePath); // call recursively
            }
            else
            {
                //sendActionMessage("No configuration selected.");
                deviceFound = false;
            }

        }
        else
        {
            deviceFound = false;
        }
    }
    else if (return_code == -2)   // board could not be opened
    {
        bool response = AlertWindow::showOkCancelBox(AlertWindow::NoIcon,
                                                     "Acquisition board not found.",
                                                     "An acquisition board could not be found. Please connect one now.",
                                                     "OK", "Cancel", 0, 0);

        if (response)
        {
            openBoard(libraryFilePath.getCharPointer()); // call recursively
        }
        else
        {
            deviceFound = false;
        }

    }

    return deviceFound;

}

bool DeviceThread::uploadBitfile(String bitfilename)
{

    deviceFound = true;

    if (!evalBoard->uploadFpgaBitfile(bitfilename.toStdString()))
    {
        LOGD("Couldn't upload bitfile from ", bitfilename);

        bool response = AlertWindow::showOkCancelBox(AlertWindow::NoIcon,
                                                     "FPGA bitfile not found.",
                                                     (evalBoard->isUSB3() ?
                                                     "The rhd2000_usb3.bit file was not found in the directory of the executable. Would you like to browse for it?" :
                                                     "The rhd2000.bit file was not found in the directory of the executable. Would you like to browse for it?"),
                                                     "Yes", "No", 0, 0);
        if (response)
        {
            // browse for file
            FileChooser fc("Select the FPGA bitfile...",
                           File::getCurrentWorkingDirectory(),
                           "*.bit",
                           true);

            if (fc.browseForFileToOpen())
            {
                File currentFile = fc.getResult();

                uploadBitfile(currentFile.getFullPathName()); // call recursively
            }
            else
            {
                deviceFound = false;
            }

        }
        else
        {
            deviceFound = false;
        }

    }

    return deviceFound;

}

void DeviceThread::initializeBoard()
{
    String bitfilename;

#if defined(__APPLE__)
    File appBundle = File::getSpecialLocation(File::currentApplicationFile);
    const String executableDirectory = appBundle.getChildFile("Contents/Resources").getFullPathName();
#else
    File executable = File::getSpecialLocation(File::currentExecutableFile);
    const String executableDirectory = executable.getParentDirectory().getFullPathName();
#endif

    bitfilename = executableDirectory;
    bitfilename += File::getSeparatorString();
    bitfilename += "shared";
    bitfilename += File::getSeparatorString();

    if (boardType == ACQUISITION_BOARD)
        bitfilename += evalBoard->isUSB3() ? "rhd2000_usb3.bit" : "rhd2000.bit";
    else if (boardType == INTAN_RHD_USB)
        bitfilename += "intan_rhd_usb.bit";
    else if (boardType == RHD_RECORDING_CONTROLLER)
        bitfilename += "intan_rec_controller.bit";

    if (!uploadBitfile(bitfilename))
    {
        return;
    }

    // Initialize the board
    LOGD("Initializing RHD2000 board.");
    evalBoard->initialize();
    // This applies the following settings:
    //  - sample rate to 30 kHz
    //  - aux command banks to zero
    //  - aux command lengths to zero
    //  - continuous run mode to 'true'
    //  - maxTimeStep to 2^32 - 1
    //  - all cable lengths to 3 feet
    //  - dspSettle to 'false'
    //  - data source mapping as 0->PortA1, 1->PortB1, 2->PortC1, 3->PortD1, etc.
    //  - enables all data streams
    //  - clears the ttlOut
    //  - disables all DACs and sets gain to 0

    setSampleRate(Rhd2000EvalBoard::SampleRate30000Hz);

    evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortA, settings.cableLength.portA);
    evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortB, settings.cableLength.portB);
    evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortC, settings.cableLength.portC);
    evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortD, settings.cableLength.portD);

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortE, settings.cableLength.portE);
        evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortF, settings.cableLength.portF);
        evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortG, settings.cableLength.portG);
        evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortH, settings.cableLength.portH);
    }

    // Select RAM Bank 0 for AuxCmd3 initially, so the ADC is calibrated.
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortA, Rhd2000EvalBoard::AuxCmd3, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortB, Rhd2000EvalBoard::AuxCmd3, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortC, Rhd2000EvalBoard::AuxCmd3, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortD, Rhd2000EvalBoard::AuxCmd3, 0);

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortE, Rhd2000EvalBoard::AuxCmd3, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortF, Rhd2000EvalBoard::AuxCmd3, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortG, Rhd2000EvalBoard::AuxCmd3, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortH, Rhd2000EvalBoard::AuxCmd3, 0);
    }

    // Since our longest command sequence is 60 commands, run the SPI interface for
    // 60 samples (64 for usb3 power-of two needs)
    evalBoard->setMaxTimeStep(INIT_STEP);
    evalBoard->setContinuousRunMode(false);

    // Start SPI interface
    evalBoard->run();

    // Wait for the 60-sample run to complete
    while (evalBoard->isRunning())
    {
        ;
    }

    // Read the resulting single data block from the USB interface. We don't
    // need to do anything with this, since it was only used for ADC calibration
    ScopedPointer<Rhd2000DataBlock> dataBlock = new Rhd2000DataBlock(evalBoard->getNumEnabledDataStreams(), evalBoard->isUSB3());

    evalBoard->readDataBlock(dataBlock, INIT_STEP);
    // Now that ADC calibration has been performed, we switch to the command sequence
    // that does not execute ADC calibration.
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortA, Rhd2000EvalBoard::AuxCmd3,
        settings.fastSettleEnabled ? 2 : 1);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortB, Rhd2000EvalBoard::AuxCmd3,
        settings.fastSettleEnabled ? 2 : 1);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortC, Rhd2000EvalBoard::AuxCmd3,
        settings.fastSettleEnabled ? 2 : 1);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortD, Rhd2000EvalBoard::AuxCmd3,
        settings.fastSettleEnabled ? 2 : 1);

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortE, Rhd2000EvalBoard::AuxCmd3,
            settings.fastSettleEnabled ? 2 : 1);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortF, Rhd2000EvalBoard::AuxCmd3,
            settings.fastSettleEnabled ? 2 : 1);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortG, Rhd2000EvalBoard::AuxCmd3,
            settings.fastSettleEnabled ? 2 : 1);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortH, Rhd2000EvalBoard::AuxCmd3,
            settings.fastSettleEnabled ? 2 : 1);
    }

    adcChannelNames.clear();
    ttlLineNames.clear();

    for (int i = 0; i < 8; i++)
    {
        adcChannelNames.add("ADC" + String(i + 1));
        ttlLineNames.add("TTL" + String(i + 1));
    }

}

void DeviceThread::scanPorts()
{
    if (!deviceFound) //Safety to avoid crashes if board not present
    {
        return;
    }

    impedanceThread->stopThreadSafely();

    //Clear previous known streams
    enabledStreams.clear();

    // Scan SPI ports
    int delay, hs, id;
    int register59Value;

    for (auto headstage : headstages)
    {
        headstage->setNumStreams(0); // reset stream count
    }

    Rhd2000EvalBoard::BoardDataSource initStreamPorts[8] =
    {
        Rhd2000EvalBoard::PortA1,
        Rhd2000EvalBoard::PortA2,
        Rhd2000EvalBoard::PortB1,
        Rhd2000EvalBoard::PortB2,
        Rhd2000EvalBoard::PortC1,
        Rhd2000EvalBoard::PortC2,
        Rhd2000EvalBoard::PortD1,
        Rhd2000EvalBoard::PortD2
        //Rhd2000EvalBoard::PortE1,
        //Rhd2000EvalBoard::PortE2,
        //Rhd2000EvalBoard::PortF1,
        //Rhd2000EvalBoard::PortF2,
        //Rhd2000EvalBoard::PortG1,
        //Rhd2000EvalBoard::PortG2,
        //Rhd2000EvalBoard::PortH1,
        //Rhd2000EvalBoard::PortH2
    };

    chipId.insertMultiple(0, -1, 8);
    Array<int> tmpChipId(chipId);

    setSampleRate(Rhd2000EvalBoard::SampleRate30000Hz, true); // set to 30 kHz temporarily

    // Enable all data streams, and set sources to cover one or two chips
    // on Ports A-D.

    // THIS IS DIFFERENT FOR RECORDING CONTROLLER:
    for (int i = 0; i < 8; i++)
        evalBoard->setDataSource(i, initStreamPorts[i]);

    for (int i = 0; i < 8; i++)
        evalBoard->enableDataStream(i, true);

    LOGD("Number of enabled data streams: ", evalBoard->getNumEnabledDataStreams());

    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortA,
        Rhd2000EvalBoard::AuxCmd3, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortB,
        Rhd2000EvalBoard::AuxCmd3, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortC,
        Rhd2000EvalBoard::AuxCmd3, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortD,
        Rhd2000EvalBoard::AuxCmd3, 0);

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortE,
            Rhd2000EvalBoard::AuxCmd3, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortF,
            Rhd2000EvalBoard::AuxCmd3, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortG,
            Rhd2000EvalBoard::AuxCmd3, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortH,
            Rhd2000EvalBoard::AuxCmd3, 0);
    }

    // Since our longest command sequence is 60 commands, we run the SPI
    // interface for 60 samples. (64 for usb3 power-of two needs)
    evalBoard->setMaxTimeStep(INIT_STEP);
    evalBoard->setContinuousRunMode(false);

    ScopedPointer<Rhd2000DataBlock> dataBlock =
        new Rhd2000DataBlock(evalBoard->getNumEnabledDataStreams(), evalBoard->isUSB3());

    Array<int> sumGoodDelays;
    sumGoodDelays.insertMultiple(0, 0, 8);

    Array<int> indexFirstGoodDelay;
    indexFirstGoodDelay.insertMultiple(0, -1, 8);

    Array<int> indexSecondGoodDelay;
    indexSecondGoodDelay.insertMultiple(0, -1, 8);

    // Run SPI command sequence at all 16 possible FPGA MISO delay settings
    // to find optimum delay for each SPI interface cable.

    LOGD( "Checking for connected amplifier chips..." );

    for (delay = 0; delay < 16; delay++)
    {
        evalBoard->setCableDelay(Rhd2000EvalBoard::PortA, delay);
        evalBoard->setCableDelay(Rhd2000EvalBoard::PortB, delay);
        evalBoard->setCableDelay(Rhd2000EvalBoard::PortC, delay);
        evalBoard->setCableDelay(Rhd2000EvalBoard::PortD, delay);

        if (boardType == RHD_RECORDING_CONTROLLER)
        {
            evalBoard->setCableDelay(Rhd2000EvalBoard::PortE, delay);
            evalBoard->setCableDelay(Rhd2000EvalBoard::PortF, delay);
            evalBoard->setCableDelay(Rhd2000EvalBoard::PortG, delay);
            evalBoard->setCableDelay(Rhd2000EvalBoard::PortH, delay);
        }

        // Start SPI interface.
        evalBoard->run();

        // Wait for the 60-sample run to complete.
        while (evalBoard->isRunning())
        {
            ;
        }
        // Read the resulting single data block from the USB interface.
        evalBoard->readDataBlock(dataBlock, INIT_STEP);

        // Read the Intan chip ID number from each RHD2000 chip found.
        // Record delay settings that yield good communication with the chip.
        for (hs = 0; hs < headstages.size(); ++hs)
        {

            id = getDeviceId(dataBlock, hs, register59Value);

            if (id == CHIP_ID_RHD2132 || id == CHIP_ID_RHD2216 ||
                (id == CHIP_ID_RHD2164 && register59Value == REGISTER_59_MISO_A))
            {
                LOGD( "Device ID found: ", id );

                sumGoodDelays.set(hs, sumGoodDelays[hs] + 1);

                if (indexFirstGoodDelay[hs] == -1)
                {
                    indexFirstGoodDelay.set(hs, delay);
                    tmpChipId.set(hs, id);
                }
                else if (indexSecondGoodDelay[hs] == -1)
                {
                    indexSecondGoodDelay.set(hs, delay);
                    tmpChipId.set(hs, id);
                }
            }
        }
    }

#if DEBUG_EMULATE_HEADSTAGES > 0
    if (tmpChipId[0] > 0)
    {
        int chipIdx = 0;
        for (int hs = 0; hs < DEBUG_EMULATE_HEADSTAGES && hs < headstages.size() ; ++hs)
        {
            if (enabledStreams.size() < MAX_NUM_DATA_STREAMS(evalBoard->isUSB3()))
            {
#ifdef DEBUG_EMULATE_64CH
                chipId.set(chipIdx++,CHIP_ID_RHD2164);
                chipId.set(chipIdx++,CHIP_ID_RHD2164_B);
                enableHeadstage(hs, true, 2, 32);
#else
                chipId.set(chipIdx++,CHIP_ID_RHD2132);
                enableHeadstage(hs, true, 1, 32);
#endif
            }
        }
        for (int i = 0; i < enabledStreams.size(); i++)
        {
            enabledStreams.set(i,Rhd2000EvalBoard::PortA1);
        }
    }

#else
    // Now, disable data streams where we did not find chips present.
    int chipIdx = 0;

    for (int hs = 0; hs < headstages.size(); ++hs)
    {
        if ((tmpChipId[hs] > 0) && (enabledStreams.size() < MAX_NUM_DATA_STREAMS))
        {
            chipId.set(chipIdx++,tmpChipId[hs]);

            LOGD("Enabling headstage ", hs);

            if (tmpChipId[hs] == CHIP_ID_RHD2164) //RHD2164
            {
                if (enabledStreams.size() < MAX_NUM_DATA_STREAMS - 1)
                {
                    enableHeadstage(hs, true, 2, 32);
                    chipId.set(chipIdx++, CHIP_ID_RHD2164_B);
                }
                else //just one stream left
                {
                    enableHeadstage(hs, true, 1, 32);
                }
            }
            else
            {
                enableHeadstage(hs, true, 1, tmpChipId[hs] == 1 ? 32:16);
            }
        }
        else
        {
            enableHeadstage(hs, false);
        }
    }
#endif
    updateBoardStreams();

    LOGD( "Number of enabled data streams: ", evalBoard->getNumEnabledDataStreams() );

    // Set cable delay settings that yield good communication with each
    // RHD2000 chip.
    Array<int> optimumDelay;

    optimumDelay.insertMultiple(0, 0, headstages.size());

    for (hs = 0; hs < headstages.size(); ++hs)
    {
        if (sumGoodDelays[hs] == 1 || sumGoodDelays[hs] == 2)
        {
            optimumDelay.set(hs,indexFirstGoodDelay[hs]);
        }
        else if (sumGoodDelays[hs] > 2)
        {
            optimumDelay.set(hs,indexSecondGoodDelay[hs]);
        }
    }

    evalBoard->setCableDelay(Rhd2000EvalBoard::PortA,
                             std::max(optimumDelay[0],optimumDelay[1]));
    evalBoard->setCableDelay(Rhd2000EvalBoard::PortB,
                             std::max(optimumDelay[2],optimumDelay[3]));
    evalBoard->setCableDelay(Rhd2000EvalBoard::PortC,
                             std::max(optimumDelay[4],optimumDelay[5]));
    evalBoard->setCableDelay(Rhd2000EvalBoard::PortD,
                             std::max(optimumDelay[6],optimumDelay[7]));

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        evalBoard->setCableDelay(Rhd2000EvalBoard::PortE,
            std::max(optimumDelay[8], optimumDelay[9]));
        evalBoard->setCableDelay(Rhd2000EvalBoard::PortF,
            std::max(optimumDelay[10], optimumDelay[11]));
        evalBoard->setCableDelay(Rhd2000EvalBoard::PortG,
            std::max(optimumDelay[12], optimumDelay[13]));
        evalBoard->setCableDelay(Rhd2000EvalBoard::PortH,
            std::max(optimumDelay[14], optimumDelay[15]));

    }

    settings.cableLength.portA =
        evalBoard->estimateCableLengthMeters(std::max(optimumDelay[0],optimumDelay[1]));
    settings.cableLength.portB =
        evalBoard->estimateCableLengthMeters(std::max(optimumDelay[2],optimumDelay[3]));
    settings.cableLength.portC =
        evalBoard->estimateCableLengthMeters(std::max(optimumDelay[4],optimumDelay[5]));
    settings.cableLength.portD =
        evalBoard->estimateCableLengthMeters(std::max(optimumDelay[6],optimumDelay[7]));

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        settings.cableLength.portE =
            evalBoard->estimateCableLengthMeters(std::max(optimumDelay[8], optimumDelay[9]));
        settings.cableLength.portF =
            evalBoard->estimateCableLengthMeters(std::max(optimumDelay[10], optimumDelay[11]));
        settings.cableLength.portG =
            evalBoard->estimateCableLengthMeters(std::max(optimumDelay[12], optimumDelay[13]));
        settings.cableLength.portH =
            evalBoard->estimateCableLengthMeters(std::max(optimumDelay[14], optimumDelay[15]));
    }

    setSampleRate(settings.savedSampleRateIndex); // restore saved sample rate

    //updateRegisters();
    //newScan = true;
}

int DeviceThread::getDeviceId(Rhd2000DataBlock* dataBlock, int stream, int& register59Value)
{
    bool intanChipPresent;

    // First, check ROM registers 32-36 to verify that they hold 'INTAN', and
    // the initial chip name ROM registers 24-26 that hold 'RHD'.
    // This is just used to verify that we are getting good data over the SPI
    // communication channel.
    intanChipPresent = ((char) dataBlock->auxiliaryData[stream][2][32] == 'I' &&
                        (char) dataBlock->auxiliaryData[stream][2][33] == 'N' &&
                        (char) dataBlock->auxiliaryData[stream][2][34] == 'T' &&
                        (char) dataBlock->auxiliaryData[stream][2][35] == 'A' &&
                        (char) dataBlock->auxiliaryData[stream][2][36] == 'N' &&
                        (char) dataBlock->auxiliaryData[stream][2][24] == 'R' &&
                        (char) dataBlock->auxiliaryData[stream][2][25] == 'H' &&
                        (char) dataBlock->auxiliaryData[stream][2][26] == 'D');

    // If the SPI communication is bad, return -1.  Otherwise, return the Intan
    // chip ID number stored in ROM regstier 63.
    if (!intanChipPresent)
    {
        register59Value = -1;
        return -1;
    }
    else
    {
        register59Value = dataBlock->auxiliaryData[stream][2][23]; // Register 59
        return dataBlock->auxiliaryData[stream][2][19]; // chip ID (Register 63)
    }
}

void DeviceThread::updateSettings(OwnedArray<ContinuousChannel>* continuousChannels,
    OwnedArray<EventChannel>* eventChannels,
    OwnedArray<SpikeChannel>* spikeChannels,
    OwnedArray<DataStream>* sourceStreams,
    OwnedArray<DeviceInfo>* devices,
    OwnedArray<ConfigurationObject>* configurationObjects)
{

    if (!deviceFound)
        return;

    continuousChannels->clear();
    eventChannels->clear();
    spikeChannels->clear();
    sourceStreams->clear();
    devices->clear();
    configurationObjects->clear();

    channelNames.clear();

    // create device
    // CODE GOES HERE

    DataStream::Settings dataStreamSettings
    {
        "Rhythm Data",
        "Continuous and event data from a device running Rhythm FPGA firmware",
        "rhythm-fpga-device.data",

        static_cast<float>(evalBoard->getSampleRate())

    };

    DataStream* stream = new DataStream(dataStreamSettings);

    sourceStreams->add(stream);

    int hsIndex = -1;

    for (auto headstage : headstages)
    {
        hsIndex++;

        if (headstage->isConnected())
        {
            for (int ch = 0; ch < headstage->getNumChannels(); ch++)
            {

                if (headstage->getHalfChannels() && ch >= 16)
                    continue;

                ContinuousChannel::Settings channelSettings{
                    ContinuousChannel::ELECTRODE,
                    headstage->getChannelName(ch),
                    "Headstage channel from a Rhythm FPGA device",
                    "rhythm-fpga-device.continuous.headstage",

                    0.195,

                    stream
                };

                continuousChannels->add(new ContinuousChannel(channelSettings));
                continuousChannels->getLast()->setUnits("uV");

                if (impedances.valid)
                {
                    continuousChannels->getLast()->impedance.magnitude = headstage->getImpedanceMagnitude(ch);
                    continuousChannels->getLast()->impedance.phase = headstage->getImpedancePhase(ch);
                }

            }
        }
    }

    if (settings.acquireAux)
    {
        int hsIndex = -1;
        
        for (auto headstage : headstages)
        {
            hsIndex++;

            if (headstage->isConnected())
            {
                for (int ch = 0; ch < 3; ch++)
                {

                    ContinuousChannel::Settings channelSettings{
                        ContinuousChannel::AUX,
                        headstage->getStreamPrefix() + "_AUX" + String(ch + 1),
                        "Aux input channel from a Rhythm FPGA device",
                        "rhythm-fpga-device.continuous.aux",

                        0.0000374,

                        stream
                    };

                    continuousChannels->add(new ContinuousChannel(channelSettings));
                    continuousChannels->getLast()->setUnits("mV");

                }
            }
        }
    }

    if (settings.acquireAdc)
    {
        for (int ch = 0; ch < 8; ch++)
        {

            String name = "ADC" + String(ch + 1);

            ContinuousChannel::Settings channelSettings{
                ContinuousChannel::ADC,
                name,
                "ADC input channel from a Rhythm FPGA device",
                "rhythm-fpga-device.continuous.adc",

                getAdcBitVolts(ch),

                stream
            };

            continuousChannels->add(new ContinuousChannel(channelSettings));
            continuousChannels->getLast()->setUnits("V");

        }
    }

    int numDigitalLines = boardType == INTAN_RHD_USB ? 16 : 8;

    LOGD("Number of digital lines enabled: ", numDigitalLines);

    EventChannel::Settings settings{
            EventChannel::Type::TTL,
            "Rhythm FPGA TTL Input",
            "Events on digital input lines of a Rhythm FPGA device",
            "rhythm-fpga-device.events",
            stream,
            numDigitalLines
    };

    eventChannels->add(new EventChannel(settings));

}

void DeviceThread::impedanceMeasurementFinished()
{

    if (impedances.valid)
    {
        LOGD( "Updating headstage impedance values" );

        for (auto hs : headstages)
        {
            if (hs->isConnected())
            {
                hs->setImpedances(impedances);
            }
        }
    }
}

void DeviceThread::saveImpedances(File& file)
{

    if (impedances.valid)
    {
        std::unique_ptr<XmlElement> xml = std::unique_ptr<XmlElement>(new XmlElement("IMPEDANCES"));

        int globalChannelNumber = -1;

        for (auto hs : headstages)
        {
            XmlElement* headstageXml = new XmlElement("HEADSTAGE");
            headstageXml->setAttribute("name", hs->getStreamPrefix());

            for (int ch = 0; ch < hs->getNumActiveChannels(); ch++)
            {
                globalChannelNumber++;

                XmlElement* channelXml = new XmlElement("CHANNEL");
                channelXml->setAttribute("name", hs->getChannelName(ch));
                channelXml->setAttribute("number", globalChannelNumber);
                channelXml->setAttribute("magnitude", hs->getImpedanceMagnitude(ch));
                channelXml->setAttribute("phase", hs->getImpedancePhase(ch));
                headstageXml->addChildElement(channelXml);
            }

            xml->addChildElement(headstageXml);
        }

        xml->writeTo(file);
    }

}

String DeviceThread::getChannelName(int i) const
{
    return channelNames[i];
}

bool DeviceThread::isAcquisitionActive() const
{
    return isTransmitting;
}

void DeviceThread::setNamingScheme(ChannelNamingScheme scheme)
{

    channelNamingScheme = scheme;

    for (auto hs : headstages)
    {
        hs->setNamingScheme(scheme);
    }
}

ChannelNamingScheme DeviceThread::getNamingScheme()
{
    return channelNamingScheme;
}

void DeviceThread::setNumChannels(int hsNum, int numChannels)
{
    if (headstages[hsNum]->getNumChannels() == 32)
    {
        if (numChannels < headstages[hsNum]->getNumChannels())
            headstages[hsNum]->setHalfChannels(true);
        else
            headstages[hsNum]->setHalfChannels(false);

        numChannelsPerDataStream.set(headstages[hsNum]->getStreamIndex(0), numChannels);
    }

    int channelIndex = 0;

    for (auto hs : headstages)
    {
        if (hs->isConnected())
        {
            hs->setFirstChannel(channelIndex);

            channelIndex += hs->getNumActiveChannels();
        }
    }
}

int DeviceThread::getHeadstageChannels (int hsNum) const
{
    return headstages[hsNum]->getNumChannels();
}


int DeviceThread::getNumChannels()
{
    int totalChannels = getNumDataOutputs(ContinuousChannel::ELECTRODE)
           + getNumDataOutputs(ContinuousChannel::AUX)
           + getNumDataOutputs(ContinuousChannel::ADC);

    return totalChannels;
}

int DeviceThread::getNumDataOutputs(ContinuousChannel::Type type)
{

    if (type == ContinuousChannel::ELECTRODE)
    {
        int totalChannels = 0;

        for (auto headstage : headstages)
        {
            if (headstage->isConnected())
            {
                totalChannels += headstage->getNumActiveChannels();
            }
        }

        return totalChannels;
    }
    if (type == ContinuousChannel::AUX)
    {
        if (settings.acquireAux)
        {
            int numAuxOutputs = 0;

            for (auto headstage : headstages)
            {
                if (headstage->isConnected())
                {
                    numAuxOutputs += 3;
                }
            }
            return numAuxOutputs;
        }
        else
        {
            return 0;
        }
    }
    if (type == ContinuousChannel::ADC)
    {
        if (settings.acquireAdc)
        {
            return 8;
        }
        else
        {
            return 0;
        }
    }

    return 0;
}


float DeviceThread::getAdcBitVolts (int chan) const
{
    if (chan < adcBitVolts.size())
    {
        return adcBitVolts[chan];
    }
    else
    {
        if (boardType == ACQUISITION_BOARD)
        {
            return 0.00015258789; // +/-5V / pow(2,16)
        }
        else if (boardType == INTAN_RHD_USB)
        {
            return 0.0000503540039;  // 3.3V / pow(2,16)
        }
    }
}

double DeviceThread::setUpperBandwidth(double upper)
{
    impedanceThread->stopThreadSafely();

    settings.dsp.upperBandwidth = upper;

    updateRegisters();

    return settings.dsp.upperBandwidth;
}


double DeviceThread::setLowerBandwidth(double lower)
{
    impedanceThread->stopThreadSafely();

    settings.dsp.lowerBandwidth = lower;

    updateRegisters();

    return settings.dsp.lowerBandwidth;
}

double DeviceThread::setDspCutoffFreq(double freq)
{
    impedanceThread->stopThreadSafely();

    settings.dsp.cutoffFreq = freq;

    updateRegisters();

    return settings.dsp.cutoffFreq;
}

double DeviceThread::getDspCutoffFreq() const
{
    return settings.dsp.cutoffFreq;
}

void DeviceThread::setDSPOffset(bool state)
{

    impedanceThread->stopThreadSafely();

    settings.dsp.enabled = state;

    updateRegisters();
}

void DeviceThread::setTTLoutputMode(bool state)
{
    settings.ttlMode = state;

    updateSettingsDuringAcquisition = true;
}

void DeviceThread::setDAChpf(float cutoff, bool enabled)
{
    settings.desiredDAChpf = cutoff;

    settings.desiredDAChpfState = enabled;

    updateSettingsDuringAcquisition = true;
}

void DeviceThread::setFastTTLSettle(bool state, int channel)
{
    settings.fastTTLSettleEnabled = state;

    settings.fastSettleTTLChannel = channel;

    updateSettingsDuringAcquisition = true;
}

int DeviceThread::setNoiseSlicerLevel(int level)
{
    settings.noiseSlicerLevel = level;

    if (deviceFound)
        evalBoard->setAudioNoiseSuppress(settings.noiseSlicerLevel);

    // Level has been checked once before this and then is checked again in setAudioNoiseSuppress.
    // This may be overkill - maybe API should change so that the final function returns the value?

    return settings.noiseSlicerLevel;
}


bool DeviceThread::foundInputSource()
{
    return deviceFound;
}

bool DeviceThread::enableHeadstage(int hsNum, bool enabled, int nStr, int strChans)
{
    LOGD("Headstage ", hsNum, ", enabled: ", enabled, ", num streams: ", nStr, ", stream channels: ", strChans);
    LOGD("Max num headstages: ", MAX_NUM_HEADSTAGES);

    if (enabled)
    {
        headstages[hsNum]->setFirstChannel(getNumDataOutputs(ContinuousChannel::ELECTRODE));
        headstages[hsNum]->setNumStreams(nStr);
        headstages[hsNum]->setChannelsPerStream(strChans);
        headstages[hsNum]->setFirstStreamIndex(enabledStreams.size());
        enabledStreams.add(headstages[hsNum]->getDataStream(0));
        numChannelsPerDataStream.add(strChans);

        if (nStr > 1)
        {
            enabledStreams.add(headstages[hsNum]->getDataStream(1));
            numChannelsPerDataStream.add(strChans);
        }
    }
    else
    {
        int idx = enabledStreams.indexOf(headstages[hsNum]->getDataStream(0));

        if (idx >= 0)
        {
            enabledStreams.remove(idx);
            numChannelsPerDataStream.remove(idx);
        }

        if (headstages[hsNum]->getNumStreams() > 1)
        {
            idx = enabledStreams.indexOf(headstages[hsNum]->getDataStream(1));
            if (idx >= 0)
            {
                enabledStreams.remove(idx);
                numChannelsPerDataStream.remove(idx);
            }
        }

        headstages[hsNum]->setNumStreams(0);
    }

    sourceBuffers[0]->resize(getNumChannels(), 10000);

    return true;
}

void DeviceThread::updateBoardStreams()
{
    for (int i = 0; i < MAX_NUM_DATA_STREAMS; i++)
    {
        if (i < enabledStreams.size())
        {
            //std::cout << "Enabling stream " << i << " with source " << enabledStreams[i] << std::endl;
            evalBoard->enableDataStream(i,true);
            evalBoard->setDataSource(i,enabledStreams[i]);
        }
        else
        {
            evalBoard->enableDataStream(i,false);
        }
    }
}

bool DeviceThread::isHeadstageEnabled(int hsNum) const
{
    return headstages[hsNum]->isConnected();
}


int DeviceThread::getActiveChannelsInHeadstage (int hsNum) const
{
    return headstages[hsNum]->getNumActiveChannels();
}

int DeviceThread::getChannelsInHeadstage (int hsNum) const
{
    return headstages[hsNum]->getNumChannels();
}

/*void DeviceThread::assignAudioOut(int dacChannel, int dataChannel)
{
    if (deviceFound)
    {
        if (dacChannel == 0)
        {
            audioOutputR = dataChannel;
            dacChannels[0] = dataChannel;
        }
        else if (dacChannel == 1)
        {
            audioOutputL = dataChannel;
            dacChannels[1] = dataChannel;
        }

        updateSettingsDuringAcquisition = true; // set a flag and take care of setting wires
        // during the updateBuffer() method
        // to avoid problems
    }

}*/

void DeviceThread::enableAuxs(bool t)
{
    settings.acquireAux = t;
    sourceBuffers[0]->resize(getNumChannels(), 10000);
    updateRegisters();
}

void DeviceThread::enableAdcs(bool t)
{
    settings.acquireAdc = t;
    sourceBuffers[0]->resize(getNumChannels(), 10000);
}

bool DeviceThread::isAuxEnabled()
{
    return settings.acquireAux;
}

void DeviceThread::setSampleRate(int sampleRateIndex, bool isTemporary)
{
    impedanceThread->stopThreadSafely();
    if (!isTemporary)
    {
        settings.savedSampleRateIndex = sampleRateIndex;
    }

    int numUsbBlocksToRead = 0; // placeholder - make this change the number of blocks that are read in DeviceThread::updateBuffer()

    Rhd2000EvalBoard::AmplifierSampleRate sampleRate; // just for local use

    switch (sampleRateIndex)
    {
        case 0:
            sampleRate = Rhd2000EvalBoard::SampleRate1000Hz;
            numUsbBlocksToRead = 1;
            settings.boardSampleRate = 1000.0f;
            break;
        case 1:
            sampleRate = Rhd2000EvalBoard::SampleRate1250Hz;
            numUsbBlocksToRead = 1;
            settings.boardSampleRate = 1250.0f;
            break;
        case 2:
            sampleRate = Rhd2000EvalBoard::SampleRate1500Hz;
            numUsbBlocksToRead = 1;
            settings.boardSampleRate = 1500.0f;
            break;
        case 3:
            sampleRate = Rhd2000EvalBoard::SampleRate2000Hz;
            numUsbBlocksToRead = 1;
            settings.boardSampleRate = 2000.0f;
            break;
        case 4:
            sampleRate = Rhd2000EvalBoard::SampleRate2500Hz;
            numUsbBlocksToRead = 1;
            settings.boardSampleRate = 2500.0f;
            break;
        case 5:
            sampleRate = Rhd2000EvalBoard::SampleRate3000Hz;
            numUsbBlocksToRead = 2;
            settings.boardSampleRate = 3000.0f;
            break;
        case 6:
            sampleRate = Rhd2000EvalBoard::SampleRate3333Hz;
            numUsbBlocksToRead = 2;
            settings.boardSampleRate = 3333.0f;
            break;
        case 7:
            sampleRate = Rhd2000EvalBoard::SampleRate4000Hz;
            numUsbBlocksToRead = 2;
            settings.boardSampleRate = 4000.0f;
            break;
        case 8:
            sampleRate = Rhd2000EvalBoard::SampleRate5000Hz;
            numUsbBlocksToRead = 3;
            settings.boardSampleRate = 5000.0f;
            break;
        case 9:
            sampleRate = Rhd2000EvalBoard::SampleRate6250Hz;
            numUsbBlocksToRead = 3;
            settings.boardSampleRate = 6250.0f;
            break;
        case 10:
            sampleRate = Rhd2000EvalBoard::SampleRate8000Hz;
            numUsbBlocksToRead = 4;
            settings.boardSampleRate = 8000.0f;
            break;
        case 11:
            sampleRate = Rhd2000EvalBoard::SampleRate10000Hz;
            numUsbBlocksToRead = 6;
            settings.boardSampleRate = 10000.0f;
            break;
        case 12:
            sampleRate = Rhd2000EvalBoard::SampleRate12500Hz;
            numUsbBlocksToRead = 7;
            settings.boardSampleRate = 12500.0f;
            break;
        case 13:
            sampleRate = Rhd2000EvalBoard::SampleRate15000Hz;
            numUsbBlocksToRead = 8;
            settings.boardSampleRate = 15000.0f;
            break;
        case 14:
            sampleRate = Rhd2000EvalBoard::SampleRate20000Hz;
            numUsbBlocksToRead = 12;
            settings.boardSampleRate = 20000.0f;
            break;
        case 15:
            sampleRate = Rhd2000EvalBoard::SampleRate25000Hz;
            numUsbBlocksToRead = 14;
            settings.boardSampleRate = 25000.0f;
            break;
        case 16:
            sampleRate = Rhd2000EvalBoard::SampleRate30000Hz;
            numUsbBlocksToRead = 16;
            settings.boardSampleRate = 30000.0f;
            break;
        default:
            sampleRate = Rhd2000EvalBoard::SampleRate10000Hz;
            numUsbBlocksToRead = 6;
            settings.boardSampleRate = 10000.0f;
    }


    // Select per-channel amplifier sampling rate.
    evalBoard->setSampleRate(sampleRate);

    LOGD( "Sample rate set to ", evalBoard->getSampleRate() );

    // Now that we have set our sampling rate, we can set the MISO sampling delay
    // which is dependent on the sample rate.
    evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortA, settings.cableLength.portA);
    evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortB, settings.cableLength.portB);
    evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortC, settings.cableLength.portC);
    evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortD, settings.cableLength.portD);

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortE, settings.cableLength.portE);
        evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortF, settings.cableLength.portF);
        evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortG, settings.cableLength.portG);
        evalBoard->setCableLengthMeters(Rhd2000EvalBoard::PortH, settings.cableLength.portH);
    }

    updateRegisters();

}

void DeviceThread::updateRegisters()
{

    if (!deviceFound) //Safety to avoid crashes loading a chain with Rythm node withouth a board
    {
        return;
    }
    // Set up an RHD2000 register object using this sample rate to
    // optimize MUX-related register settings.
    chipRegisters.defineSampleRate(settings.boardSampleRate);

    int commandSequenceLength;
    std::vector<int> commandList;

    // Create a command list for the AuxCmd1 slot.  This command sequence will continuously
    // update Register 3, which controls the auxiliary digital output pin on each RHD2000 chip.
    // In concert with the v1.4 Rhythm FPGA code, this permits real-time control of the digital
    // output pin on chips on each SPI port.
    chipRegisters.setDigOutLow();   // Take auxiliary output out of HiZ mode.
    commandSequenceLength = chipRegisters.createCommandListUpdateDigOut(commandList);
    evalBoard->uploadCommandList(commandList, Rhd2000EvalBoard::AuxCmd1, 0);
    evalBoard->selectAuxCommandLength(Rhd2000EvalBoard::AuxCmd1, 0, commandSequenceLength - 1);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortA, Rhd2000EvalBoard::AuxCmd1, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortB, Rhd2000EvalBoard::AuxCmd1, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortC, Rhd2000EvalBoard::AuxCmd1, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortD, Rhd2000EvalBoard::AuxCmd1, 0);

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortE, Rhd2000EvalBoard::AuxCmd1, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortF, Rhd2000EvalBoard::AuxCmd1, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortG, Rhd2000EvalBoard::AuxCmd1, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortH, Rhd2000EvalBoard::AuxCmd1, 0);

    }

    // Next, we'll create a command list for the AuxCmd2 slot.  This command sequence
    // will sample the temperature sensor and other auxiliary ADC inputs.
    commandSequenceLength = chipRegisters.createCommandListTempSensor(commandList);
    evalBoard->uploadCommandList(commandList, Rhd2000EvalBoard::AuxCmd2, 0);
    evalBoard->selectAuxCommandLength(Rhd2000EvalBoard::AuxCmd2, 0, commandSequenceLength - 1);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortA, Rhd2000EvalBoard::AuxCmd2, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortB, Rhd2000EvalBoard::AuxCmd2, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortC, Rhd2000EvalBoard::AuxCmd2, 0);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortD, Rhd2000EvalBoard::AuxCmd2, 0);

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortE, Rhd2000EvalBoard::AuxCmd2, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortF, Rhd2000EvalBoard::AuxCmd2, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortG, Rhd2000EvalBoard::AuxCmd2, 0);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortH, Rhd2000EvalBoard::AuxCmd2, 0);
    }

    // Before generating register configuration command sequences, set amplifier
    // bandwidth paramters.
    settings.dsp.cutoffFreq = chipRegisters.setDspCutoffFreq(settings.dsp.cutoffFreq);
    settings.dsp.lowerBandwidth = chipRegisters.setLowerBandwidth(settings.dsp.lowerBandwidth);
    settings.dsp.upperBandwidth = chipRegisters.setUpperBandwidth(settings.dsp.upperBandwidth);
    chipRegisters.enableDsp(settings.dsp.enabled);

    // enable/disable aux inputs:
    chipRegisters.enableAux1(settings.acquireAux);
    chipRegisters.enableAux2(settings.acquireAux);
    chipRegisters.enableAux3(settings.acquireAux);

    chipRegisters.createCommandListRegisterConfig(commandList, true);
    // Upload version with ADC calibration to AuxCmd3 RAM Bank 0.
    evalBoard->uploadCommandList(commandList, Rhd2000EvalBoard::AuxCmd3, 0);
    evalBoard->selectAuxCommandLength(Rhd2000EvalBoard::AuxCmd3, 0,
                                      commandSequenceLength - 1);

    commandSequenceLength = chipRegisters.createCommandListRegisterConfig(commandList, false);
    // Upload version with no ADC calibration to AuxCmd3 RAM Bank 1.
    evalBoard->uploadCommandList(commandList, Rhd2000EvalBoard::AuxCmd3, 1);
    evalBoard->selectAuxCommandLength(Rhd2000EvalBoard::AuxCmd3, 0,
                                      commandSequenceLength - 1);


    chipRegisters.setFastSettle(true);

    commandSequenceLength = chipRegisters.createCommandListRegisterConfig(commandList, false);
    // Upload version with fast settle enabled to AuxCmd3 RAM Bank 2.
    evalBoard->uploadCommandList(commandList, Rhd2000EvalBoard::AuxCmd3, 2);
    evalBoard->selectAuxCommandLength(Rhd2000EvalBoard::AuxCmd3, 0,
                                      commandSequenceLength - 1);

    chipRegisters.setFastSettle(false);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortA, Rhd2000EvalBoard::AuxCmd3,
                                    settings.fastSettleEnabled ? 2 : 1);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortB, Rhd2000EvalBoard::AuxCmd3,
                                    settings.fastSettleEnabled ? 2 : 1);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortC, Rhd2000EvalBoard::AuxCmd3,
                                    settings.fastSettleEnabled ? 2 : 1);
    evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortD, Rhd2000EvalBoard::AuxCmd3,
                                    settings.fastSettleEnabled ? 2 : 1);

    if (boardType == RHD_RECORDING_CONTROLLER)
    {
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortE, Rhd2000EvalBoard::AuxCmd3,
            settings.fastSettleEnabled ? 2 : 1);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortF, Rhd2000EvalBoard::AuxCmd3,
            settings.fastSettleEnabled ? 2 : 1);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortG, Rhd2000EvalBoard::AuxCmd3,
            settings.fastSettleEnabled ? 2 : 1);
        evalBoard->selectAuxCommandBank(Rhd2000EvalBoard::PortH, Rhd2000EvalBoard::AuxCmd3,
            settings.fastSettleEnabled ? 2 : 1);
    }
}

void DeviceThread::setCableLength(int hsNum, float length)
{
    // Set the MISO sampling delay, which is dependent on the sample rate.

    switch (hsNum)
    {
        case 0:
            evalBoard->setCableLengthFeet(Rhd2000EvalBoard::PortA, length);
            break;
        case 1:
            evalBoard->setCableLengthFeet(Rhd2000EvalBoard::PortB, length);
            break;
        case 2:
            evalBoard->setCableLengthFeet(Rhd2000EvalBoard::PortC, length);
            break;
        case 3:
            evalBoard->setCableLengthFeet(Rhd2000EvalBoard::PortD, length);
            break;
        case 4:
            evalBoard->setCableLengthFeet(Rhd2000EvalBoard::PortE, length);
            break;
        case 5:
            evalBoard->setCableLengthFeet(Rhd2000EvalBoard::PortF, length);
            break;
        case 6:
            evalBoard->setCableLengthFeet(Rhd2000EvalBoard::PortG, length);
            break;
        case 7:
            evalBoard->setCableLengthFeet(Rhd2000EvalBoard::PortH, length);
            break;
        default:
            break;
    }

}

bool DeviceThread::startAcquisition()
{
    if (!deviceFound || (getNumChannels() == 0))
        return false;

    impedanceThread->waitSafely();
    dataBlock = new Rhd2000DataBlock(evalBoard->getNumEnabledDataStreams(), evalBoard->isUSB3());

    LOGD( "Expecting ", getNumChannels() ," channels." );

    if (boardType == ACQUISITION_BOARD)
    {
        int ledArray[8] = {1, 1, 0, 0, 0, 0, 0, 0};
        evalBoard->setLedDisplay(ledArray);
    }

    // reset TTL output state
    for (int k = 0; k < 16; k++)
    {
        TTL_OUTPUT_STATE[k] = 0;
    }

    //LOGD( "Number of 16-bit words in FIFO: ", evalBoard->numWordsInFifo());
    //LOGD("Is eval board running: ", evalBoard->isRunning());

    //LOGD("RHD2000 data thread starting acquisition.");

    if (1)
    {
        //LOGD("Flushing FIFO.");
        evalBoard->flush();
        evalBoard->setContinuousRunMode(true);
        evalBoard->run();
    }

    blockSize = dataBlock->calculateDataBlockSizeInWords(evalBoard->getNumEnabledDataStreams(), evalBoard->isUSB3());
    //LOGD("Expecting blocksize of ", blockSize, " for ", evalBoard->getNumEnabledDataStreams(), " streams");

    startThread();

    isTransmitting = true;

    return true;
}

bool DeviceThread::stopAcquisition()
{

    //LOGD("RHD2000 data thread stopping acquisition.");

    if (isThreadRunning())
    {
        signalThreadShouldExit();

    }

    if (waitForThreadToExit(500))
    {
        //LOGD("RHD2000 data thread exited.");
    }
    else
    {
        //LOGD("RHD2000 data thread failed to exit, continuing anyway...");
    }

    if (deviceFound)
    {
        evalBoard->setContinuousRunMode(false);
        evalBoard->setMaxTimeStep(0);
        LOGD( "Flushing FIFO." );
        evalBoard->flush();
    }

    sourceBuffers[0]->clear();

    if (deviceFound && boardType == ACQUISITION_BOARD)
    {
        LOGD( "Number of 16-bit words in FIFO: ", evalBoard->numWordsInFifo() );

        int ledArray[8] = {1, 0, 0, 0, 0, 0, 0, 0};
        evalBoard->setLedDisplay(ledArray);
    }

    isTransmitting = false;
    updateSettingsDuringAcquisition = false;

    // remove timers
    digitalOutputTimers.clear();

    // remove commands
    while (!digitalOutputCommands.empty())
        digitalOutputCommands.pop();

    return true;
}

bool DeviceThread::updateBuffer()
{
    unsigned char* bufferPtr;
    double ts;

    if (evalBoard->isUSB3() || evalBoard->numWordsInFifo() >= blockSize)
    {
        bool return_code;

        return_code = evalBoard->readRawDataBlock(&bufferPtr);
        // see Rhd2000DataBlock::fillFromUsbBuffer() for documentation of buffer structure

        int index = 0;
        int auxIndex, chanIndex;
        int numStreams = enabledStreams.size();
        int nSamps = Rhd2000DataBlock::getSamplesPerDataBlock(evalBoard->isUSB3());
        //dspw LOGC("nSamps = ", nSamps);  // dspw
        //evalBoard->printFIFOmetrics();
        for (int samp = 0; samp < nSamps; samp++)
        {
            int channel = -1;

            if (!Rhd2000DataBlock::checkUsbHeader(bufferPtr, index))
            {
                LOGE( "Error in Rhd2000EvalBoard::readDataBlock: Incorrect header." );
                break;
            }

            index += 8; // magic number header width (bytes)
            int64 timestamp = Rhd2000DataBlock::convertUsbTimeStamp(bufferPtr, index);
            index += 4; // timestamp width
            auxIndex = index; // aux chans start at this offset
            index += 6 * numStreams; // width of the 3 aux chans

            for (int dataStream = 0; dataStream < numStreams; dataStream++)
            {                
                
                int nChans = numChannelsPerDataStream[dataStream];

                chanIndex = index + 2*dataStream;
                
                if ((chipId[dataStream] == CHIP_ID_RHD2132) && (nChans == 16)) //RHD2132 16ch. headstage
                {
                    chanIndex += 2 * RHD2132_16CH_OFFSET * numStreams;
                }
                
                for (int chan = 0; chan < nChans; chan++)
                {
                    channel++;
                    thisSample[channel] = float(*(uint16*)(bufferPtr + chanIndex) - 32768) * 0.195f;
                    chanIndex += 2 * numStreams; // single chan width (2 bytes)
                }
                
            }
            index += 64 * numStreams; // neural data width
            auxIndex += 2 * numStreams; // skip AuxCmd1 slots (see updateRegisters())
            // copy the 3 aux channels
            if (settings.acquireAux)
            {
                for (int dataStream = 0; dataStream < numStreams; dataStream++)
                {
                    if (chipId[dataStream] != CHIP_ID_RHD2164_B)
                    {
                        int auxNum = (samp+3) % 4;
                        if (auxNum < 3)
                        {
                            auxSamples[dataStream][auxNum] = float(*(uint16*)(bufferPtr + auxIndex) - 32768)*0.0000374;
                        }
                        for (int chan = 0; chan < 3; chan++)
                        {
                            channel++;
                            if (auxNum == 3)
                            {
                                auxBuffer[channel] = auxSamples[dataStream][chan];
                            }
                            thisSample[channel] = auxBuffer[channel];
                        }
                    }
                    auxIndex += 2; // single chan width (2 bytes)
                }
            }
            index += 2 * numStreams; // skip over filler word at the end of each data stream
            // copy the 8 ADC channels
            if (settings.acquireAdc)
            {
                for (int adcChan = 0; adcChan < 8; ++adcChan)
                {

                    channel++;
                    // ADC waveform units = volts

                    if (boardType == ACQUISITION_BOARD)
                    {
                        thisSample[channel] = adcRangeSettings[adcChan] == 0 ?
                            0.00015258789 * float(*(uint16*)(bufferPtr + index)) - 5 - 0.4096 : // account for +/-5V input range and DC offset
                            0.00030517578 * float(*(uint16*)(bufferPtr + index)); // shouldn't this be half the value, not 2x?
                    }
                    else if (boardType == INTAN_RHD_USB) {
                        thisSample[channel] = 0.000050354 * float(*(uint16*)(bufferPtr + index));
                    }
                    index += 2; // single chan width (2 bytes)
                }
            }
            else
            {
                index += 16; // skip ADC chans (8 * 2 bytes)
            }

            uint64 ttlEventWord = *(uint64*)(bufferPtr + index) & 65535;
            ttlEventWord = ttlEventWord |  eventState;
            //LOGC("eventWord = ", ttlEventWord);
         
            index += 4;

            sourceBuffers[0]->addToBuffer(thisSample,
                                          &timestamp,
                                          &ts,
                                          &ttlEventWord,
                                          1);
        }

    }


    if (updateSettingsDuringAcquisition)
    {
        LOGD( "DAC" );
        for (int k=0; k<8; k++)
        {
            if (dacChannelsToUpdate[k])
            {
                dacChannelsToUpdate[k] = false;
                if (dacChannels[k] >= 0)
                {
                    evalBoard->enableDac(k, true);
                    evalBoard->selectDacDataStream(k, dacStream[k]);
                    evalBoard->selectDacDataChannel(k, dacChannels[k]);
                    evalBoard->setDacThreshold(k, (int)abs((dacThresholds[k]/0.195) + 32768),dacThresholds[k] >= 0);
                   // evalBoard->setDacThresholdVoltage(k, (int) dacThresholds[k]);
                }
                else
                {
                    evalBoard->enableDac(k, false);
                }
            }
        }

        evalBoard->setTtlMode(settings.ttlMode ? 1 : 0);
        evalBoard->enableExternalFastSettle(settings.fastTTLSettleEnabled);
        evalBoard->setExternalFastSettleChannel(settings.fastSettleTTLChannel);
        evalBoard->setDacHighpassFilter(settings.desiredDAChpf);
        evalBoard->enableDacHighpassFilter(settings.desiredDAChpfState);
        evalBoard->enableBoardLeds(settings.ledsEnabled);
        evalBoard->setClockDivider(settings.clockDivideFactor);

        updateSettingsDuringAcquisition = false;
    }

    if (!digitalOutputCommands.empty())
    {

        while (!digitalOutputCommands.empty())
        {
            DigitalOutputCommand command = digitalOutputCommands.front();
            TTL_OUTPUT_STATE[command.ttlLine] = command.state;
            digitalOutputCommands.pop();

        }

        evalBoard->setTtlOut(TTL_OUTPUT_STATE);

        LOGB("TTL OUTPUT STATE: ",
            TTL_OUTPUT_STATE[0],
            TTL_OUTPUT_STATE[1],
            TTL_OUTPUT_STATE[2],
            TTL_OUTPUT_STATE[3],
            TTL_OUTPUT_STATE[4],
            TTL_OUTPUT_STATE[5],
            TTL_OUTPUT_STATE[6],
            TTL_OUTPUT_STATE[7]);

    }

    return true;

}

int DeviceThread::getChannelFromHeadstage (int hs, int ch)
{
    int channelCount = 0;
    int hsCount = 0;
    if (hs < 0 || hs >= headstages.size() + 1)
        return -1;
    if (hs == headstages.size()) //let's consider this the ADC channels
    {
        int adcOutputs = getNumDataOutputs(ContinuousChannel::ADC);

        if (adcOutputs> 0)
        {
            return getNumDataOutputs(ContinuousChannel::ELECTRODE) + getNumDataOutputs(ContinuousChannel::AUX) + ch;
        }
        else
            return -1;
    }
    if (headstages[hs]->isConnected())
    {
        if (ch < 0)
            return -1;
        if (ch < headstages[hs]->getNumActiveChannels())
        {
            for (int i = 0; i < hs; i++)
            {
                channelCount += headstages[i]->getNumActiveChannels();
            }
            return channelCount + ch;
        }
        else if (ch < headstages[hs]->getNumActiveChannels() + 3)
        {
            for (int i = 0; i < headstages.size(); i++)
            {
                if (headstages[i]->isConnected())
                {
                    channelCount += headstages[i]->getNumActiveChannels();
                    if (i < hs)
                        hsCount++;
                }
            }
            return channelCount + hsCount * 3 + ch-headstages[hs]->getNumActiveChannels();
        }
        else
        {
            return -1;
        }

    }
    else
    {
        return -1;
    }
}

Array<const Headstage*> DeviceThread::getConnectedHeadstages()
{
    Array<const Headstage*> headstageArray;

    for (auto hs : headstages)
    {
        if (hs->isConnected())
            headstageArray.add(hs);
    }

    return headstageArray;
}

int DeviceThread::getHeadstageChannel (int& hs, int ch) const
{
    int channelCount = 0;
    int hsCount = 0;

    if (ch < 0)
        return -1;

    for (int i = 0; i < headstages.size(); i++)
    {
        if (headstages[i]->isConnected())
        {
            int chans = headstages[i]->getNumActiveChannels();

            if (ch >= channelCount && ch < channelCount + chans)
            {
                hs = i;
                return ch - channelCount;
            }
            channelCount += chans;
            hsCount++;
        }
    }
    if (ch < (channelCount + hsCount * 3)) //AUX
    {
        hsCount = (ch - channelCount) / 3;

        for (int i = 0; i < headstages.size(); i++)
        {
            if (headstages[i]->isConnected())
            {
                if (hsCount == 0)
                {
                    hs = i;
                    return ch - channelCount;
                }
                hsCount--;
                channelCount++;
            }
        }
    }
    return -1;
}

void DeviceThread::enableBoardLeds(bool enable)
{

    if (!deviceFound)
        return;

    settings.ledsEnabled = enable;

    if (isAcquisitionActive())
        updateSettingsDuringAcquisition = true;
    else
        evalBoard->enableBoardLeds(enable);
}

int DeviceThread::setClockDivider(int divide_ratio)
{
    if (!deviceFound)
        return 1;

    // Divide ratio should be 1 or an even number
    if (divide_ratio != 1 && divide_ratio % 2)
        divide_ratio--;

    // Format the divide ratio from its true value to the
    // format required by the firmware
    // Ratio    N
    // 1        0
    // >=2      Ratio/2
    if (divide_ratio == 1)
        settings.clockDivideFactor = 0;
    else
        settings.clockDivideFactor = static_cast<uint16>(divide_ratio/2);

    if (isAcquisitionActive())
        updateSettingsDuringAcquisition = true;
    else
        evalBoard->setClockDivider(settings.clockDivideFactor);

    return divide_ratio;
}

void DeviceThread::setAdcRange(int channel, short range)
{
    adcRangeSettings[channel] = range;
}

short DeviceThread::getAdcRange(int channel) const
{
    return adcRangeSettings[channel];
}

void DeviceThread::runImpedanceTest()
{

    impedanceThread->stopThreadSafely();

    impedanceThread->runThread();

}




/* C:B**************************************************************************
This software is Copyright 2014-2017 Bright Plaza Inc. <drivetrust@drivetrust.com>

This file is part of sedutil.

sedutil is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

sedutil is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with sedutil.  If not, see <http://www.gnu.org/licenses/>.

* C:E********************************************************************** */
#pragma once
#include "os.h"
#include <stdio.h>
#include <iostream>
#pragma warning(push)
#pragma warning(disable : 4091)
#include <Ntddscsi.h>
#pragma warning(pop)
#include <winioctl.h>
#include <vector>
#include "DtaDiskUSB.h"
#include "DtaEndianFixup.h"
#include "DtaStructures.h"
#include "DtaHexDump.h"


/** Close the filehandle so this object can be delete. */
DtaDiskUSB::~DtaDiskUSB()
{
    LOG(D1) << "Destroying DtaDiskUSB";
    _aligned_free(scsiPointer);
}

void DtaDiskUSB::init(const char * devref)
{
    LOG(D1) << "Creating DtaDiskUSB::DtaDiskUSB() " << devref;
    SDWB * scsi =
            (SDWB *) _aligned_malloc((sizeof (SDWB)), 4096);
    scsiPointer = (void *) scsi;
     hDev = CreateFile(devref,
                      GENERIC_WRITE | GENERIC_READ,
                      FILE_SHARE_WRITE | FILE_SHARE_READ,
                      NULL,
                      OPEN_EXISTING,
                      0,
                      NULL);
    if (INVALID_HANDLE_VALUE == hDev)
        return;
    else
        isOpen = TRUE;
}

uint8_t DtaDiskUSB::sendCmd(ATACOMMAND cmd, uint8_t protocol, uint16_t comID,
                        void * buffer, uint32_t bufferlen)
{
    if(isSAS) {
        return(sendCmd_SAS(cmd, protocol, comID, buffer, bufferlen));
    }

    LOG(D1) << "Entering DtaDiskUSB::sendCmd";
    DWORD bytesReturned = 0; // data returned
    if (!isOpen) {
        LOG(D1) << "Device open failed";
        return DTAERROR_OPEN_ERR; //disk open failed so this will too
    }
    /*
     * Initialize the SCSI_PASS_THROUGH_DIRECT structures
     * per windows DOC with the SCSI Command set reference
     */
    SDWB * scsi = (SDWB *) scsiPointer;
    memset(scsi, 0, sizeof (SDWB));
    scsi->sd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    scsi->sd.SenseInfoOffset = offsetof(SDWB, sensebytes);
    scsi->sd.ScsiStatus = 0;
    scsi->sd.PathId = 0;
    scsi->sd.TargetId = 0;
    scsi->sd.Lun = 0;
    scsi->sd.DataBuffer = buffer;
    scsi->sd.DataTransferLength = bufferlen;
    scsi->sd.TimeOutValue = 20;
    scsi->sd.SenseInfoLength = 32;
    scsi->sd.CdbLength = 12;
    scsi->sd.Cdb[0] = 0xa1; //ATA PASS THROUGH
    scsi->sd.Cdb[9] = (UCHAR) cmd;

    if (IF_RECV == cmd) {
        /* security protocol in */
        scsi->sd.DataIn = SCSI_IOCTL_DATA_IN;
        scsi->sd.Cdb[1] = 4 << 1;  // PIO IN
        scsi->sd.Cdb[2] = 0x0e;
        scsi->sd.Cdb[4] = (UCHAR) (bufferlen / 512);
        scsi->sd.Cdb[3] = protocol;
        scsi->sd.Cdb[7] = ((comID & 0xff00) >> 8); // Commid MSB
        scsi->sd.Cdb[6] = (comID & 0x00ff); // Commid LSB

    }
    else if (IDENTIFY == cmd) {

        /* Inquiry command */
        scsi->sd.DataTransferLength = 512;
        scsi->sd.DataIn = SCSI_IOCTL_DATA_IN;
        scsi->sd.Cdb[1] = 4 << 1;  // PIO IN
        scsi->sd.Cdb[2] = 0x0e;
        scsi->sd.Cdb[4] = 1;
    }
    else {
        /* security protocol out */
        scsi->sd.DataIn = SCSI_IOCTL_DATA_OUT;
        scsi->sd.Cdb[1] = 5 << 1;  // PIO OUT
        scsi->sd.Cdb[2] = 0x06;
        scsi->sd.Cdb[4] = (UCHAR)(bufferlen / 512);
        scsi->sd.Cdb[3] = protocol;
        scsi->sd.Cdb[7] = ((comID & 0xff00) >> 8); // Commid MSB
        scsi->sd.Cdb[6] = (comID & 0x00ff); // Commid LSB
    }

    DeviceIoControl(hDev, // device to be queried
                    IOCTL_SCSI_PASS_THROUGH_DIRECT, // operation to perform
                    scsi, sizeof (SDWB),
                    scsi, sizeof (SDWB),
                    &bytesReturned, // # bytes returned
                    (LPOVERLAPPED) NULL); // synchronous I/O
     return (scsi->sd.ScsiStatus);
}

/** adds the IDENTIFY information to the disk_info structure */

void DtaDiskUSB::identify(OPAL_DiskInfo& disk_info)
{
    identify_SAS(disk_info);
    if (DEVICE_TYPE_OTHER!=disk_info.devType) {
        return;
    }

    LOG(D1) << "Entering DtaDiskUSB::identify()";
    std::vector<uint8_t> nullz(512, 0x00);
    void* identifyResp{nullptr};
    identifyResp = _aligned_malloc(MIN_BUFFER_LENGTH, IO_BUFFER_ALIGNMENT);
    if (nullptr == identifyResp) {
        return;
    }
    memset(identifyResp, 0, MIN_BUFFER_LENGTH);
    uint8_t iorc = sendCmd(IDENTIFY, 0x00, 0x0000, identifyResp, MIN_BUFFER_LENGTH);

    if (0x00 != iorc) {
        LOG(D) << "IDENTIFY Failed " << (uint16_t) iorc;
    }
	if (!(memcmp(identifyResp, nullz.data(), 512))) {
		disk_info.devType = DEVICE_TYPE_OTHER;
		_aligned_free(identifyResp);
		return;
	}
	UASP_INQUIRY_RESPONSE * id = (UASP_INQUIRY_RESPONSE *) identifyResp;
    disk_info.devType = DEVICE_TYPE_USB;
    for (int i = 0; i < sizeof (disk_info.serialNum); i += 2) {
        disk_info.serialNum[i] = id->ProductSerial[i + 1];
        disk_info.serialNum[i + 1] = id->ProductSerial[i];
    }
    //memcpy(disk_info.serialNum, id->ProductSerial, sizeof(disk_info.serialNum));
    for (int i = 0; i < sizeof (disk_info.firmwareRev); i += 2) {
        disk_info.firmwareRev[i] = id->ProductRev[i + 1];
        disk_info.firmwareRev[i + 1] = id->ProductRev[i];
    }
    //memcpy(disk_info.firmwareRev, id->ProductRev, sizeof(disk_info.firmwareRev));
    for (int i = 0; i < sizeof (disk_info.modelNum); i += 2) {
        disk_info.modelNum[i] = id->ProductID[i + 1];
        disk_info.modelNum[i + 1] = id->ProductID[i];
    }
    //memcpy(disk_info.modelNum, id->ProductID, sizeof(disk_info.modelNum));
    _aligned_free(identifyResp);
    return;
}

uint8_t DtaDiskUSB::sendCmd_SAS(ATACOMMAND cmd, uint8_t protocol, uint16_t comID,
                         void * buffer, uint32_t bufferlen)
{
    LOG(D1) << "Entering DtaDiskUSB::sendCmd_SAS";
    DWORD bytesReturned = 0; // data returned
    if (!isOpen) {
        LOG(D1) << "Device open failed";
        return DTAERROR_OPEN_ERR; //disk open failed so this will too
    }
    /*
     * Initialize the SCSI_PASS_THROUGH_DIRECT structures
     * per windows DOC with the SCSI Command set reference
     */
    SDWB * scsi = (SDWB *) scsiPointer;
    memset(scsi, 0, sizeof (SDWB));
    scsi->sd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    scsi->sd.SenseInfoOffset = offsetof(SDWB, sensebytes);
    scsi->sd.ScsiStatus = 0;
    scsi->sd.PathId = 0;
    scsi->sd.TargetId = 0;
    scsi->sd.Lun = 0;
    scsi->sd.DataBuffer = buffer;
    scsi->sd.DataTransferLength = bufferlen;
    scsi->sd.TimeOutValue = 20;
    scsi->sd.SenseInfoLength = 32;
    scsi->sd.CdbLength = 12;

    // initialize SCSI CDB
    switch(cmd)
    {
        default:
        {
            return 0xff;
        }
        case IF_RECV:
        {
            scsi->sd.DataIn = SCSI_IOCTL_DATA_IN;
            //auto * p = (CScsiCmdSecurityProtocolIn *) &(scsi->sd.Cdb[0]);
            //p->m_Opcode = p->OPCODE;
            scsi->sd.Cdb[0] = 0xA2;
            //p->m_SecurityProtocol = protocol;
            scsi->sd.Cdb[1] = protocol;
            //p->m_SecurityProtocolSpecific = SWAP16(comID);
            scsi->sd.Cdb[2] = ((comID & 0xff00) >> 8); // Commid MSB
            scsi->sd.Cdb[3] = (comID & 0x00ff); // Commid LSB
            //p->m_INC_512 = 1;
            scsi->sd.Cdb[4] = 0x80;
            //p->m_AllocationLength = SWAP32(bufferlen/512);
            uint32_t cnt = bufferlen/512;
            scsi->sd.Cdb[6] = UCHAR(cnt >> 24);
            scsi->sd.Cdb[7] = UCHAR(cnt >> 16);
            scsi->sd.Cdb[8] = UCHAR(cnt >> 8);
            scsi->sd.Cdb[9] = UCHAR(cnt);
            break;
        }
        case IF_SEND:
        {
            scsi->sd.DataIn = SCSI_IOCTL_DATA_OUT;
            //auto * p = (CScsiCmdSecurityProtocolOut *) &(scsi->sd.Cdb[0]);
            //p->m_Opcode = p->OPCODE;
            scsi->sd.Cdb[0] = 0xB5;
            //p->m_SecurityProtocol = protocol;
            scsi->sd.Cdb[1] = protocol;
            //p->m_SecurityProtocolSpecific = SWAP16(comID);
            scsi->sd.Cdb[2] = ((comID & 0xff00) >> 8); // Commid MSB
            scsi->sd.Cdb[3] = (comID & 0x00ff); // Commid LSB
            //p->m_INC_512 = 1;
            scsi->sd.Cdb[4] = 0x80;
            //p->m_TransferLength = SWAP32(bufferlen/512);
            uint32_t cnt = bufferlen/512;
            scsi->sd.Cdb[6] = UCHAR(cnt >> 24);
            scsi->sd.Cdb[7] = UCHAR(cnt >> 16);
            scsi->sd.Cdb[8] = UCHAR(cnt >> 8);
            scsi->sd.Cdb[9] = UCHAR(cnt);
            break;
        }
        case IDENTIFY:
        {
            scsi->sd.DataIn = SCSI_IOCTL_DATA_IN;
            scsi->sd.Cdb[0] = 0x12; //INQUIRY
            scsi->sd.Cdb[4] = 0x28; //sizeof(CScsiCmdInquiry_StandardData);     // some incorrect packing happens on Windows
            break;
        }
    }

    LOG(D4) << "CDB before";
    IFLOG(D4) DtaHexDump(scsi->sd.Cdb, 12);

    DeviceIoControl(hDev, // device to be queried
                    IOCTL_SCSI_PASS_THROUGH_DIRECT, // operation to perform
                    scsi, sizeof (SDWB),
                    scsi, sizeof (SDWB),
                    &bytesReturned, // # bytes returned
                    (LPOVERLAPPED) NULL); // synchronous I/O
     return (scsi->sd.ScsiStatus);
}

static void safecopy(uint8_t* dst, size_t dstsize, uint8_t* src, size_t srcsize)
{
    const size_t size = min(dstsize, srcsize);
    if (size > 0)
    {
        memcpy(dst, src, size);
    }
    if (size < dstsize)
    {
        memset(dst + size, '\0', dstsize - size);
    }
}

void DtaDiskUSB::identify_SAS(OPAL_DiskInfo& disk_info)
{
    LOG(D1) << "Entering DtaDiskUSB::identify_SAS()";
    std::vector<uint8_t> nullz(512, 0x00);
    void* identifyResp{nullptr};
    identifyResp = _aligned_malloc(MIN_BUFFER_LENGTH, IO_BUFFER_ALIGNMENT);
    if (nullptr == identifyResp)
    {
        return;
    }
    memset(identifyResp, 0, MIN_BUFFER_LENGTH);
    uint8_t iorc = sendCmd_SAS(IDENTIFY, 0x00, 0x0000, identifyResp, MIN_BUFFER_LENGTH);

    if (0x00 != iorc) {
        LOG(D) << "IDENTIFY Failed " << (uint16_t) iorc;
    }
    if (!(memcmp(identifyResp, nullz.data(), 512))) {
        disk_info.devType = DEVICE_TYPE_OTHER;
        _aligned_free(identifyResp);
        return;
    }

    disk_info.devType = DEVICE_TYPE_USB;
    isSAS = 1;

    // response is a standard INQUIRY (at least 36 bytes)
    // some incorrect packing happens on Windows
    //auto resp = (CScsiCmdInquiry_StandardData *) identifyResp;
    auto resp = (uint8_t *)identifyResp;

    // fill out disk info fields
    //safecopy(disk_info.serialNum, sizeof(disk_info.serialNum), resp->m_T10VendorId, sizeof(resp->m_T10VendorId));
    safecopy(disk_info.serialNum, sizeof(disk_info.serialNum), resp+0x8, 8);
    //safecopy(disk_info.firmwareRev, sizeof(disk_info.firmwareRev), resp->m_ProductRevisionLevel, sizeof(resp->m_ProductRevisionLevel));
    safecopy(disk_info.firmwareRev, sizeof(disk_info.firmwareRev), resp+0x20, 4);
    //safecopy(disk_info.modelNum, sizeof(disk_info.modelNum), resp->m_ProductId, sizeof(resp->m_ProductId));
    safecopy(disk_info.modelNum, sizeof(disk_info.modelNum), resp+0x10, 0x10);

    _aligned_free(identifyResp);
    return;
}

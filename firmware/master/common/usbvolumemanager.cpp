#include "usbvolumemanager.h"
#include "elfprogram.h"
#include "bits.h"
#include "flash_volume.h"
#include "flash_volumeheader.h"
#include "flash_syslfs.h"

#ifndef SIFTEO_SIMULATOR
#include "usb/usbdevice.h"
#endif

FlashVolumeWriter UsbVolumeManager::writer;

void UsbVolumeManager::onUsbData(const USBProtocolMsg &m)
{
    USBProtocolMsg reply(USBProtocol::Installer);
    switch (m.header & 0xff) {

    case WriteGameHeader: {
        /*
         * Install a game volume, removing other games which match the same package string.
         */

        if (m.payloadLen() < 5)
            break;

        const uint32_t numBytes = *reinterpret_cast<const uint32_t*>(m.payload);
        const char* packageStr = reinterpret_cast<const char*>(m.payload + sizeof(numBytes));
        if (!memchr(packageStr, 0, m.payloadLen() - 4))
            break;

        if (writer.beginGame(numBytes, packageStr)) {
            reply.header |= WroteHeaderOK;
        } else {
            reply.header |= WroteHeaderFail;
        }
        break;
    }

    case WriteLauncherHeader: {
        /*
         * Install a launcher volume, removing other launchers first.
         */

        if (m.payloadLen() < 4)
            break;

        const uint32_t numBytes = *reinterpret_cast<const uint32_t*>(m.payload);
        if (writer.beginLauncher(numBytes)) {
            reply.header |= WroteHeaderOK;
        } else {
            reply.header |= WroteHeaderFail;
        }
        break;
    }

    case WritePayload:
        writer.appendPayload(m.payload, m.payloadLen());
        return;

    case WriteCommit:
        writer.commit();
        return;

    case VolumeOverview:
        volumeOverview(reply);
        break;

    case VolumeDetail:
        volumeDetail(m, reply);
        break;

    case DeleteVolume: {
        if (m.payloadLen() < sizeof(unsigned))
            break;

        unsigned volBlockCode = *m.castPayload<unsigned>();
        FlashVolume vol = FlashMapBlock::fromCode(volBlockCode);

        if (vol.isValid()) {
            vol.deleteTree();
            reply.header |= DeleteVolume;
        }
        break;
    }

    case DeleteSysLFS: {
        SysLFS::deleteAll();
        reply.header |= DeleteSysLFS;
        break;
    }

    case VolumeMetadata:
        volumeMetadata(m, reply);
        break;

    case DeleteEverything: {
        FlashVolume::deleteEverything();
        reply.header |= DeleteEverything;
        break;
    }

    case FirmwareVersion: {
        reply.header |= FirmwareVersion;
        reply.append((const uint8_t*) TOSTRING(SDK_VERSION), sizeof(TOSTRING(SDK_VERSION)));
        break;
    }

    case PairCube:
        pairCube(m, reply);
        break;

    }

#ifndef SIFTEO_SIMULATOR
    UsbDevice::write(reply.bytes, reply.len);
#endif
}

void UsbVolumeManager::volumeOverview(USBProtocolMsg &reply)
{
    /*
     * Retrieve top-level info about all volumes.
     * We treat deleted, incomplete, and LFS volumes specially here.
     */

    VolumeOverviewReply *r = reply.zeroCopyAppend<VolumeOverviewReply>();
    reply.header |= VolumeOverview;

    r->systemBytes = 0;
    r->freeBytes = FlashDevice::CAPACITY;
    r->bits.clear();

    FlashVolumeIter vi;
    FlashVolume vol;

    vi.begin();
    while (vi.next(vol)) {
        FlashBlockRef ref;
        FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, vol.block);
        ASSERT(hdr->isHeaderValid());

        // Ignore deleted/incomplete volumes. (Treat them as free space)
        if (hdr->isDeleted())
            continue;

        // All other volumes count against our free space
        unsigned volSize = hdr->volumeSizeInBytes();
        r->freeBytes -= volSize;

        // Count space used by SysLFS
        if (hdr->parentBlock == 0 && hdr->type == FlashVolume::T_LFS) {
            r->systemBytes += volSize;
            continue;
        }

        // Other top-level volumes are marked in the bitmap
        if (hdr->parentBlock == 0)
            r->bits.mark(vol.block.code);
    }
}

void UsbVolumeManager::volumeDetail(const USBProtocolMsg &m, USBProtocolMsg &reply)
{
    /*
     * Read fixed-size details pertaining to a single volume.
     */

    if (m.payloadLen() < sizeof(unsigned))
        return;

    unsigned volBlockCode = *m.castPayload<unsigned>();
    FlashVolume vol = FlashMapBlock::fromCode(volBlockCode);

    if (vol.isValid()) {
        VolumeDetailReply *r = reply.zeroCopyAppend<VolumeDetailReply>();
        reply.header |= VolumeDetail;

        // Map the volume header
        FlashBlockRef ref;
        FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, vol.block);
        ASSERT(hdr->isHeaderValid());

        r->type = hdr->type;
        r->selfBytes = hdr->volumeSizeInBytes();
        r->childBytes = 0;

        // Total up the size of all child volumes

        FlashVolumeIter vi;
        FlashVolume child;
        vi.begin();
        while (vi.next(child)) {
            if (child.getParent().block.code == volBlockCode) {
                hdr = FlashVolumeHeader::get(ref, child.block);
                ASSERT(hdr->isHeaderValid());
                r->childBytes += hdr->volumeSizeInBytes();
            }
        }
    }
}

void UsbVolumeManager::volumeMetadata(const USBProtocolMsg &m, USBProtocolMsg &reply)
{
    /*
     * Read a single metadata key from a volume.
     * Returns an empty buffer on error (bad volume, not an ELF, key not found)
     */

    if (m.payloadLen() < sizeof(VolumeMetadataRequest))
        return;

    const VolumeMetadataRequest *req = m.castPayload<VolumeMetadataRequest>();
    FlashVolume vol = FlashMapBlock::fromCode(req->volume);
    FlashBlockRef volRef;
    Elf::Program program;

    reply.header |= VolumeMetadata;

    if (vol.isValid() && program.init(vol.getPayload(volRef))) {
        FlashBlockRef metaRef;
        uint32_t size;

        const uint8_t *meta = (const uint8_t*) program.getMeta(metaRef, req->key, 1, size);
        if (meta) {
            size = MIN(size, reply.bytesFree());
            reply.append(meta, size);
        }
    }
}

void UsbVolumeManager::pairCube(const USBProtocolMsg &m, USBProtocolMsg &reply)
{
    /*
     * Directly add pairing info for the given cube hardware ID, without going
     * through the neighbor/RF based pairing process.
     *
     * Note that this will only affect the base's normal pairing process
     * after a restart, since CubeConnector only loads pairing IDs once
     * during init.
     *
     * Useful to inject pairing information at the factory, for instance.
     */

    const PairCubeRequest *payload = m.castPayload<PairCubeRequest>();

    reply.header |= PairCube;

    SysLFS::PairingIDRecord rec;
    rec.load();

    if (payload->pairingSlot >= arraysize(rec.hwid)) {
        ASSERT(0);
        return;
    }

    rec.hwid[payload->pairingSlot] = payload->hwid;
    if (!SysLFS::write(SysLFS::kPairingID, rec)) {
        ASSERT(0);
    }
}

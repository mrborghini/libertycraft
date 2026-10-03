package dev.libertycraft.world;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import dev.libertycraft.link.Link;
import dev.libertycraft.link.Proto;
import org.junit.jupiter.api.Test;

class HostDriveTest {
	@Test
	void parsesMountTypes() {
		assertEquals(HostDrive.MountType.BOAT, HostDrive.MountType.parse("boat", HostDrive.MountType.NONE));
		assertEquals(HostDrive.MountType.BOAT, HostDrive.MountType.parse(" Oak_Boat ", HostDrive.MountType.NONE));
		assertEquals(HostDrive.MountType.HORSE, HostDrive.MountType.parse("HORSE", HostDrive.MountType.BOAT));
		assertEquals(HostDrive.MountType.MINECART, HostDrive.MountType.parse("minecart", HostDrive.MountType.BOAT));
		assertEquals(HostDrive.MountType.NONE, HostDrive.MountType.parse("none", HostDrive.MountType.BOAT));
		assertEquals(HostDrive.MountType.NONE, HostDrive.MountType.parse("", HostDrive.MountType.BOAT));
		assertEquals(HostDrive.MountType.BOAT, HostDrive.MountType.parse("pig", HostDrive.MountType.BOAT));
		assertEquals(HostDrive.MountType.BOAT, HostDrive.MountType.parse(null, HostDrive.MountType.BOAT));
	}

	@Test
	void skyStateFlags() {
		Link.SkyState sky = new Link.SkyState();
		sky.flags = Proto.SKY_IN_GAME;
		assertFalse(sky.hostDrives());
		assertFalse(sky.inVehicle());
		sky.flags = Proto.SKY_IN_GAME | Proto.SKY_HOST_DRIVES;
		assertTrue(sky.hostDrives());
		assertFalse(sky.inVehicle());
		sky.flags = Proto.SKY_IN_GAME | Proto.SKY_HOST_DRIVES | Proto.SKY_IN_VEHICLE;
		assertTrue(sky.inVehicle());
		// In-vehicle means nothing without host-drives (the protocol pairs them).
		sky.flags = Proto.SKY_IN_GAME | Proto.SKY_IN_VEHICLE;
		assertFalse(sky.inVehicle());
		// The protocol header's bits.
		assertEquals(1 << 3, Proto.SKY_HOST_DRIVES);
		assertEquals(1 << 4, Proto.SKY_IN_VEHICLE);
	}
}

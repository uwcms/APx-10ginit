#include "mdio.h"
#include "stdio.h"
#include <fcntl.h>
#include <libeasymem.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Enable debug printf?
#undef ENABLE_DEBUG_PRINT

#ifdef ENABLE_DEBUG_PRINT
#define dprintf(...) printf(__VA_ARGS__)
#else
#define dprintf(...)
#endif

// MDIO operations
#define MDIO_OP_ADDR 0 // send 16-bit address operation
#define MDIO_OP_WR 1   // send 16-bit write value
#define MDIO_OP_RD 3   // receive 16-bit read value

// MDIO peripheral register offsets
#define MDIO_ADDRESS1_OFFSET 0x00  // address1 contains the OP, Port Addr, Device Addr
#define MDIO_ADDRESS2_OFFSET 0x04  // address2 contains the 16-bit register address
#define MDIO_WRITE_BUF_OFFSET 0x08 // write_buf contains the 16-bit write value
#define MDIO_READ_BUF_OFFSET 0x0c  // read_buf contains the 16-bit read value
#define MDIO_CTRL_REG_OFFSET 0x10  // control register

// control reg mask bits
#define MDIO_CTRL_ENA_BIT 0x8      // bit set to 1 to enable mdio peripheral
#define MDIO_CTRL_REQ_BUSY_BIT 0x1 // bit set to 1 to start MDIO request, stays 1 to indicate busy

// address1 register fields
#define MDIO_ADDRESS1_OP_MASK 0x3    // operation is 2-bit field
#define MDIO_ADDRESS1_OP_POSN 10     // operation bit position
#define MDIO_ADDRESS1_PORT_MASK 0x1f // port address is 5-bit field
#define MDIO_ADDRESS1_PORT_POSN 5    // port address bit position
#define MDIO_ADDRESS1_DEV_MASK 0x1f  // device address is 5-bit position
#define MDIO_ADDRESS1_DEV_POSN 0     // device address position

#define MDIO_ADDR1(OPCODE, PORT, DEVICE)                             \
	(((OPCODE & MDIO_ADDRESS1_OP_MASK) << MDIO_ADDRESS1_OP_POSN) +   \
	 ((PORT & MDIO_ADDRESS1_PORT_MASK) << MDIO_ADDRESS1_PORT_POSN) + \
	 ((DEVICE & MDIO_ADDRESS1_DEV_MASK) << MDIO_ADDRESS1_DEV_POSN))

#define MDIO_STATUS_POLL_LIMIT 100   // maximum # of status reads waiting for xation to complete
#define MDIO_STATUS_POLL_DELAY 10000 // µs between polls of the status register

static int mdio_phy_addr(void *mdioDevice, uint32_t PortAddr, uint32_t DeviceAddr, uint32_t RegAddr) {
	uint32_t regval;

	dprintf("Setting address for transfer...\n");

	// Set opcode, port, dev
	regval = MDIO_ADDR1(MDIO_OP_ADDR, PortAddr, DeviceAddr);
	dprintf("addr1 = MDIO_ADDR1(MDIO_OP_ADDR, PortAddr, DeviceAddr) = %04x\n", regval);
	if (easymem_safewrite32(mdioDevice, MDIO_ADDRESS1_OFFSET, 1, &regval, 0) < 0)
		return -1;

	usleep(10000);

	// Set register address
	dprintf("addr2 = %04x\n", RegAddr);
	if (easymem_safewrite32(mdioDevice, MDIO_ADDRESS2_OFFSET, 1, &RegAddr, 0) < 0)
		return -1;

	usleep(10000);
	// Initiate transfer
	regval = MDIO_CTRL_ENA_BIT | MDIO_CTRL_REQ_BUSY_BIT;
	if (easymem_safewrite32(mdioDevice, MDIO_CTRL_REG_OFFSET, 1, &regval, 0) < 0)
		return -1;
	usleep(10000);

	// Wait for the BUSY bit to clear.
	int i;
	for (i = 0; i < MDIO_STATUS_POLL_LIMIT; i++) {
		if (easymem_saferead32(mdioDevice, MDIO_CTRL_REG_OFFSET, 1, &regval, 0) < 0)
			return -1;
		if (!(regval & MDIO_CTRL_REQ_BUSY_BIT))
			break;
		usleep(MDIO_STATUS_POLL_DELAY);
	}
	if (i == MDIO_STATUS_POLL_LIMIT) {
		dprintf("Failed. Timeout. Status register 0x%08x\n", regval);
		return -1;
	}
	dprintf("Address for transfer set.\n");

	return 0;
}

int mdio_phy_write(void *mdioDevice, uint32_t PortAddr, uint32_t DeviceAddr, uint32_t RegAddr, uint32_t WriteBuf) {
	// function to write a physical address in a PHY device over MDIO
	// returns 0 if successful, -1 if not
	// The MDIO write operation actually happens over multiple serial transactions
	// The first transaction transfers the register address
	// The second transaction transfers the write data

	if (mdio_phy_addr(mdioDevice, PortAddr, DeviceAddr, RegAddr) < 0)
		return -1;

	uint32_t regval;

	dprintf("Writing PHY register...\n");

	// Set opcode, port, dev
	regval = MDIO_ADDR1(MDIO_OP_WR, PortAddr, DeviceAddr);
	if (easymem_safewrite32(mdioDevice, MDIO_ADDRESS1_OFFSET, 1, &regval, 0) < 0)
		return -1;

	// Set data to be written
	if (easymem_safewrite32(mdioDevice, MDIO_WRITE_BUF_OFFSET, 1, &WriteBuf, 0) < 0)
		return -1;

	// Initiate transfer
	regval = MDIO_CTRL_ENA_BIT | MDIO_CTRL_REQ_BUSY_BIT;
	if (easymem_safewrite32(mdioDevice, MDIO_CTRL_REG_OFFSET, 1, &regval, 0) < 0)
		return -1;

	// Wait for the BUSY bit to clear.
	int i;
	for (i = 0; i < MDIO_STATUS_POLL_LIMIT; i++) {
		if (easymem_saferead32(mdioDevice, MDIO_CTRL_REG_OFFSET, 1, &regval, 0) < 0)
			return -1;
		if (!(regval & MDIO_CTRL_REQ_BUSY_BIT))
			break;
		usleep(MDIO_STATUS_POLL_DELAY);
	}
	if (i == MDIO_STATUS_POLL_LIMIT)
		return -1;

	dprintf("PHY register written.\n");

	return 0;
}

int mdio_phy_read(void *mdioDevice, uint32_t PortAddr, uint32_t DeviceAddr, uint32_t RegAddr, uint32_t *pReadBuf) {
	// function to read a physical address in a PHY device over MDIO
	// returns 0 if successful, -1 if not
	// The MDIO read operation actually happens over multiple serial transactions
	// The first transaction transfers the register address
	// The second transaction transfers the read data
	// After the read transfer is complete the MDIO read buffer is read

	if (mdio_phy_addr(mdioDevice, PortAddr, DeviceAddr, RegAddr) < 0)
		return -1;

	uint32_t regval;

	dprintf("Reading PHY register...\n");

	// Set opcode, port, dev
	regval = MDIO_ADDR1(MDIO_OP_RD, PortAddr, DeviceAddr);
	if (easymem_safewrite32(mdioDevice, MDIO_ADDRESS1_OFFSET, 1, &regval, 0) < 0)
		return -1;

	// Initiate transfer
	regval = MDIO_CTRL_ENA_BIT | MDIO_CTRL_REQ_BUSY_BIT;
	if (easymem_safewrite32(mdioDevice, MDIO_CTRL_REG_OFFSET, 1, &regval, 0) < 0)
		return -1;

	// Wait for the BUSY bit to clear.
	int i;
	for (i = 0; i < MDIO_STATUS_POLL_LIMIT; i++) {
		if (easymem_saferead32(mdioDevice, MDIO_CTRL_REG_OFFSET, 1, &regval, 0) < 0)
			return -1;
		if (!(regval & MDIO_CTRL_REQ_BUSY_BIT))
			break;
		usleep(MDIO_STATUS_POLL_DELAY);
	}
	if (i == MDIO_STATUS_POLL_LIMIT)
		return -1;

	// Get the result data.
	if (easymem_saferead32(mdioDevice, MDIO_READ_BUF_OFFSET, 1, pReadBuf, 0) < 0)
		return -1;

	dprintf("PHY register read.\n");

	return 0;
}

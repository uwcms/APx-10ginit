#ifndef _MDIO_H
#define _MDIO_H

#include "stdint.h"

int mdio_phy_write(void *mdioDevice, uint32_t PortAddr, uint32_t DeviceAddr, uint32_t RegAddr, uint32_t WriteBuf);
int mdio_phy_read(void *mdioDevice, uint32_t PortAddr, uint32_t DeviceAddr, uint32_t RegAddr, uint32_t *pReadBuf);

#endif

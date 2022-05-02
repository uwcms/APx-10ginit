#include "inicpp.h"
#include "mdio.h"
#include "stdio.h"
#include <fcntl.h>
#include <getopt.h>
#include <gpiod.h>
#include <libeasymem.h>
#include <libwisci2c.h>
#include <list>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * How long to wait after changing a GBE reset value.
 * 10,000 µs is insufficient
 * 100,000 µs would need more experimentation
 * 1,000,000 µs seems to work
 */
#define GBE_RST_SETTLE_US 1000000

#define GBE_REG_USR_MAC_HIGH 0x00
#define GBE_REG_USR_MAC_LOW 0x04
#define GBE_REG_USR_IP 0x08
#define GBE_REG_TEST_REG 0x0c
#define GBE_REG_SYSTEM_MAC_HIGH 0x10
#define GBE_REG_SYSTEM_MAC_LOW 0x14
#define GBE_REG_USR_MAC_CFG 0x18

std::string format_mac(const uint8_t macbuf[6]) {
	char macstr[20];
	if (snprintf(macstr, 20, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx", macbuf[0], macbuf[1], macbuf[2], macbuf[3], macbuf[4], macbuf[5]) != 17)
		throw std::runtime_error("Unable to format MAC address");
	return macstr;
}

class MDIO_Operation {
public:
	uint32_t port;
	uint32_t dev;
	uint32_t reg;
	uint32_t val;
	MDIO_Operation(uint32_t port, uint32_t dev, uint32_t reg, uint32_t val) : port(port), dev(dev), reg(reg), val(val) {}
};

std::list<MDIO_Operation> parse_mdio_writes(const std::string &mdio_config_str) {
	std::list<MDIO_Operation> ops;
	for (int i = 0; i < mdio_config_str.size();) {
		uint32_t port;
		uint32_t dev;
		uint32_t reg;
		uint32_t val;
		int incr = 0;
		if (sscanf(mdio_config_str.substr(i).c_str(), " %d.%d:%x=%x %n", &port, &dev, &reg, &val, &incr) != 4)
			break;
		ops.emplace_back(port, dev, reg, val);
		i += incr;
	}
	return ops;
}

bool validate_mac_address(std::string valid_mac_prefix, const uint8_t macbuf[6], bool warn = true) {
	if (!valid_mac_prefix.size()) {
		if (warn)
			fprintf(stderr, "Warning: config.valid_mac_address_prefix is not set!\n");
		return true;
	}
	std::string fmac = format_mac(macbuf);
	if (fmac == "00:00:00:00:00:00") {
		fprintf(stderr, "Error: MAC address %s is not valid!  It cannot be 00:00:00:00:00:00.\n", fmac.c_str());
		return false;
	}
	if (fmac == "ff:ff:ff:ff:ff:ff") {
		fprintf(stderr, "Error: MAC address %s is not valid!  It cannot be ff:ff:ff:ff:ff:ff (the broadcast MAC address).\n", fmac.c_str());
		return false;
	}
	if (macbuf[0] & 0x01) {
		fprintf(stderr, "Error: MAC address %s is not valid!  It cannot be a multicast MAC address.\n", fmac.c_str());
		return false;
	}
	if (fmac.substr(0, valid_mac_prefix.size()) != valid_mac_prefix) {
		fprintf(stderr, "Error: MAC address %s is not valid!  It must begin with \"%s\" (config.valid_mac_address_prefix).\n", fmac.c_str(), valid_mac_prefix.c_str());
		return false;
	}
	return true;
}

int main(int argc, char *argv[]) {

	// Parse arguments

	const char *usage = "%s [options]\n"
	                    "\n"
	                    "-c FILENAME  -- The configuration file specifying relevant device paths.\n"
	                    "\n"
	                    "Actions (choose one):\n"
	                    "  -q         -- Query the stored MAC address.\n"
	                    "  -s MAC     -- Update the stored MAC address.\n"
	                    "  -i         -- Initialize the 10GbE core.\n"
	                    "\n";

	std::string config_file_path = "/etc/10ginit.ini";
	enum {
		DO_NOTHING = 0,
		DO_QUERY,
		DO_STORE,
		DO_INITIALIZE,
	} action;
	std::string set_mac;

	int opt;
	while ((opt = getopt(argc, argv, "c:qs:i")) != -1) {
		switch (opt) {
		case 'c':
			config_file_path = optarg;
			break;

		case 'q':
			if (action != DO_NOTHING) {
				fprintf(stderr, usage, argv[0]);
				return 1;
			}
			action = DO_QUERY;
			break;
		case 's':
			if (action != DO_NOTHING) {
				fprintf(stderr, usage, argv[0]);
				return 1;
			}
			action = DO_STORE;
			set_mac = optarg;
			break;
		case 'i':
			if (action != DO_NOTHING) {
				fprintf(stderr, usage, argv[0]);
				return 1;
			}
			action = DO_INITIALIZE;
			break;
		default: /* '?' */
			fprintf(stderr, usage, argv[0]);
			return 1;
		}
	}
	if (action == DO_NOTHING) {
		fprintf(stderr, usage, argv[0]);
		return 1;
	}

	ini::IniFile config;
	config.load(config_file_path.c_str());

	// Map 10GbE UIO registers.

	std::string gbe_uio_path = config["resources"]["gbe_uio"].as<std::string>();

	void *gbe_registers = NULL;
	if (easymem_map_uio(&gbe_registers, gbe_uio_path.c_str(), 0, 0x1000, 0) < 0) {
		perror("Mapping GBE UIO device");
		return 0;
	}

	// Map MDIO UIO registers.

	std::string mdio_uio_path = config["resources"]["mdio_uio"].as<std::string>();
	std::string mdio_reg_writes = config["config"]["mdio_reg_writes"].as<std::string>();

	void *mdio_registers = NULL;
	if (mdio_reg_writes.size()) {
		if (easymem_map_uio(&mdio_registers, mdio_uio_path.c_str(), 0, 0x1000, 0) < 0) {
			perror("Mapping MDIO UIO device");
			return 0;
		}
	}

	// Open reset GPIO

	std::string reset_gpio = config["resources"]["reset_gpio"].as<std::string>();
	unsigned int reset_gpio_bit = config["resources"]["reset_gpio_bit"].as<unsigned int>();

	char *reset_gpiochip_path = realpath(reset_gpio.c_str(), NULL);
	if (reset_gpiochip_path == NULL) {
		perror("unable to resolve reset gpio chip path");
		return 1;
	}
	struct gpiod_chip *gpiochip = gpiod_chip_open(reset_gpiochip_path);
	free(reset_gpiochip_path);
	if (gpiochip == NULL) {
		perror("error opening reset gpio chip");
		return 1;
	}
	struct gpiod_line *gpio_line_reset = gpiod_chip_get_line(gpiochip, reset_gpio_bit);
	if (gpio_line_reset == NULL) {
		perror("error opening reset gpio line");
		return 1;
	}

	// Open MAC EEPROM I2C

	std::string mac_eeprom_bus = config["resources"]["mac_eeprom_bus"].as<std::string>();
	unsigned int mac_eeprom_address = config["resources"]["mac_eeprom_address"].as<unsigned int>();
	unsigned int mac_eeprom_offset = config["resources"]["mac_eeprom_offset"].as<unsigned int>();

	std::string valid_mac_prefix = config["config"]["valid_mac_address_prefix"].as<std::string>();

	int mac_eeprom_i2cfd = open(mac_eeprom_bus.c_str(), O_RDWR);
	if (mac_eeprom_i2cfd < 0) {
		perror("error opening mac eeprom");
		return 1;
	}

	// Begin work.

	// Read the MAC in advance.
	uint8_t macbuf[6];
	if (i2c_read(mac_eeprom_i2cfd, mac_eeprom_address, mac_eeprom_offset, macbuf, 6) != 6) {
		perror("error reading mac eeprom");
		return 1;
	}

	if (action == DO_QUERY) {
		printf("%s\n", format_mac(macbuf).c_str());
		if (!validate_mac_address(valid_mac_prefix, macbuf, false))
			return 1;
	}
	else if (action == DO_STORE) {
		uint8_t macbuf2[6];
		if (sscanf(set_mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &macbuf2[0], &macbuf2[1], &macbuf2[2], &macbuf2[3], &macbuf2[4], &macbuf2[5]) != 6) {
			fprintf(stderr, "Unable to parse input MAC address.\n");
			return 1;
		}
		if (!validate_mac_address(valid_mac_prefix, macbuf2))
			return 1;
		if (i2c_write(mac_eeprom_i2cfd, mac_eeprom_address, mac_eeprom_offset, macbuf2, 6) != 6) {
			perror("error writing mac eeprom");
			return 1;
		}
		sleep(1);
		if (i2c_read(mac_eeprom_i2cfd, mac_eeprom_address, mac_eeprom_offset, macbuf, 6) != 6) {
			perror("error reading mac eeprom to confirm");
			return 1;
		}
		if (memcmp(macbuf, macbuf2, 6) != 0) {
			fprintf(stderr, "MAC address read back does not match MAC address stored.\n");
			return 1;
		}
	}
	else if (action == DO_INITIALIZE) {
		// Get the reset output pin and ensure the core is in reset.
		if (gpiod_line_request_output(gpio_line_reset, "10ginit", 1) < 0) {
			perror("unable to access reset gpio");
			return 1;
		}
		if (gpiod_line_set_value(gpio_line_reset, 1) < 0) {
			perror("unable to assert reset gpio");
			return 1;
		}
		usleep(GBE_RST_SETTLE_US); // Allow reset to 'settle'.

		// Write PHY configuration via MDIO, if present.

		std::list<MDIO_Operation> mdio_ops = parse_mdio_writes(mdio_reg_writes);
		for (auto mdio_op : mdio_ops) {
#if 0
			uint32_t readout = 0xffffffff;
			if (mdio_phy_read(mdio_registers, mdio_op.port, mdio_op.dev, mdio_op.reg, &readout) < 0) {
				fprintf(stderr, "Failed to read MDIO register.\n");
				return 1;
			}
			printf("Read (Pre) 0x%04x\n", readout);
#endif
			printf("Writing MDIO: port %u, dev %u, reg 0x%04x = 0x%04x\n", mdio_op.port, mdio_op.dev, mdio_op.reg, mdio_op.val);
			if (mdio_phy_write(mdio_registers, mdio_op.port, mdio_op.dev, mdio_op.reg, mdio_op.val) < 0) {
				fprintf(stderr, "Failed to write MDIO register.\n");
				return 1;
			}
#if 0
			if (mdio_phy_read(mdio_registers, mdio_op.port, mdio_op.dev, mdio_op.reg, &readout) < 0) {
				fprintf(stderr, "Failed to read MDIO register.\n");
				return 1;
			}
			printf("Read (Post) 0x%04x\n", readout);
#endif
		}

		// Resume 10GbE configuration.

		printf("Configuring 10GbE with MAC address %s\n", format_mac(macbuf).c_str());

		if (!validate_mac_address(valid_mac_prefix, macbuf)) {
			fprintf(stderr, "Leaving 10GbE core in reset.\n");
			return 1;
		}

		// Set the MAC address registers.
		uint32_t mac_high_regval = 0;
		uint32_t mac_low_regval = 0;
		for (int i = 0; i < 4; ++i)
			mac_high_regval |= (macbuf[i] << (8 * i));
		for (int i = 4; i < 6; ++i)
			mac_low_regval |= (macbuf[i] << (8 * (i - 4)));

		if (easymem_safewrite32(gbe_registers, GBE_REG_USR_MAC_HIGH, 1, &mac_high_regval, 0) < 0) {
			fprintf(stderr, "Unable to write GBE_REG_USR_MAC_HIGH.\n");
			return 1;
		}
		if (easymem_safewrite32(gbe_registers, GBE_REG_USR_MAC_LOW, 1, &mac_low_regval, 0) < 0) {
			fprintf(stderr, "Unable to write GBE_REG_USR_MAC_LOW.\n");
			return 1;
		}
		usleep(GBE_RST_SETTLE_US); // Allow configuration to 'settle' before valid-ing it.
		uint32_t regval = 1;
		if (easymem_safewrite32(gbe_registers, GBE_REG_USR_MAC_CFG, 1, &regval, 0) < 0) {
			fprintf(stderr, "Unable to write GBE_REG_USR_MAC_CFG.\n");
			return 1;
		}

		if (gpiod_line_set_value(gpio_line_reset, 0) < 0) {
			perror("unable to release reset gpio");
			return 1;
		}
		usleep(GBE_RST_SETTLE_US); // Allow reset to 'settle'.

		uint32_t mac_high_regval2 = 0xffffffff;
		uint32_t mac_low_regval2 = 0xffffffff;
		if (easymem_saferead32(gbe_registers, GBE_REG_SYSTEM_MAC_HIGH, 1, &mac_high_regval2, 0) < 0) {
			fprintf(stderr, "Unable to read GBE_REG_USR_SYSTEM_HIGH.\n");
			return 1;
		}
		if (easymem_saferead32(gbe_registers, GBE_REG_SYSTEM_MAC_LOW, 1, &mac_low_regval2, 0) < 0) {
			fprintf(stderr, "Unable to read GBE_REG_USR_SYSTEM_LOW.\n");
			return 1;
		}

		if (mac_high_regval != mac_high_regval2 || (mac_low_regval & 0x0000ffff) != (mac_low_regval2 & 0x0000ffff)) {
			uint8_t macbuf2[6];
			for (int i = 0; i < 4; ++i)
				macbuf2[i] = (mac_high_regval2 >> (8 * i)) & 0xff;
			for (int i = 4; i < 6; ++i)
				macbuf2[i] = (mac_low_regval2 >> (8 * (i - 4))) & 0xff;
			fprintf(stderr, "10GbE core configuration failed: Configured MAC address %s.  Read back MAC addresss %s.\n", format_mac(macbuf).c_str(), format_mac(macbuf2).c_str());
			fprintf(stderr, "Putting core back into reset.\n");
			usleep(1000000);
			printf("0x%04x\n", UNSAFE_REG32(gbe_registers, GBE_REG_SYSTEM_MAC_LOW) & 0x0000ffff);

			if (gpiod_line_set_value(gpio_line_reset, 1) < 0) {
				perror("unable to assert reset gpio");
				return 1;
			}

			return 1;
		}
	}

	return 0;
}

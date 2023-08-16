#include <Wire.h>


#define PUT_UINT32_LE(field, value)            \
    do {                                       \
        (field)[0] = (uint8_t)((value) >> 0);  \
        (field)[1] = (uint8_t)((value) >> 8);  \
        (field)[2] = (uint8_t)((value) >> 16); \
        (field)[3] = (uint8_t)((value) >> 24); \
    } while (0)


static void board_i2c_pinmux_init(void)
{
    GLB_GPIO_Type pinlist[] = {
        GLB_GPIO_PIN_14,
        GLB_GPIO_PIN_15
    };
    GLB_GPIO_Func_Init(GPIO_FUN_I2C0, pinlist, 2);
}

static inline bool bflb_i2c_isend(struct bflb_device_s *dev)
{
    uint32_t regval;
    uint32_t reg_base;

    reg_base = dev->reg_base;

    regval = getreg32(reg_base + I2C_INT_STS_OFFSET);

    if (regval & I2C_END_INT) {
        return true;
    }

    return false;
}

static inline bool bflb_i2c_isnak(struct bflb_device_s *dev)
{
    uint32_t regval;
    uint32_t reg_base;

    reg_base = dev->reg_base;

    regval = getreg32(reg_base + I2C_INT_STS_OFFSET);

    if (regval & I2C_NAK_INT) {
        return true;
    }

    return false;
}

static inline bool bflb_i2c_isbusy(struct bflb_device_s *dev)
{
    uint32_t regval;
    uint32_t reg_base;

    reg_base = dev->reg_base;

    regval = getreg32(reg_base + I2C_BUS_BUSY_OFFSET);

    if (regval & I2C_STS_I2C_BUS_BUSY) {
        return true;
    }

    return false;
}

static inline void bflb_i2c_enable(struct bflb_device_s *dev)
{
    uint32_t regval;
    uint32_t reg_base;

    reg_base = dev->reg_base;

    regval = getreg32(reg_base + I2C_CONFIG_OFFSET);
    regval |= I2C_CR_I2C_M_EN;
    putreg32(regval, reg_base + I2C_CONFIG_OFFSET);
}

static inline bool bflb_i2c_isenable(struct bflb_device_s *dev)
{
    uint32_t regval;
    uint32_t reg_base;

    reg_base = dev->reg_base;

    regval = getreg32(reg_base + I2C_CONFIG_OFFSET);
    if (regval & I2C_CR_I2C_M_EN) {
        return true;
    }

    return false;
}

static inline void bflb_i2c_disable(struct bflb_device_s *dev)
{
    uint32_t regval;
    uint32_t reg_base;

    reg_base = dev->reg_base;

    regval = getreg32(reg_base + I2C_CONFIG_OFFSET);
    regval &= ~I2C_CR_I2C_M_EN;
    putreg32(regval, reg_base + I2C_CONFIG_OFFSET);
    /* Clear I2C fifo */
    regval = getreg32(reg_base + I2C_FIFO_CONFIG_0_OFFSET);
    regval |= I2C_TX_FIFO_CLR;
    regval |= I2C_RX_FIFO_CLR;
    putreg32(regval, reg_base + I2C_FIFO_CONFIG_0_OFFSET);
    /* Clear I2C interrupt status */
    regval = getreg32(reg_base + I2C_INT_STS_OFFSET);
    regval |= I2C_CR_I2C_END_CLR;
    regval |= I2C_CR_I2C_NAK_CLR;
    regval |= I2C_CR_I2C_ARB_CLR;
    putreg32(regval, reg_base + I2C_INT_STS_OFFSET);
}


static void bflb_i2c_addr_config(struct bflb_device_s *dev, uint16_t slaveaddr, uint16_t subaddr, uint8_t subaddr_size, bool is_addr_10bit)
{
    uint32_t regval;
    uint32_t reg_base;

    reg_base = dev->reg_base;

    regval = getreg32(reg_base + I2C_CONFIG_OFFSET);

    if (subaddr_size > 0) {
        regval |= I2C_CR_I2C_SUB_ADDR_EN;
        regval &= ~I2C_CR_I2C_SUB_ADDR_BC_MASK;
        regval |= ((subaddr_size - 1) << I2C_CR_I2C_SUB_ADDR_BC_SHIFT);
    } else {
        regval &= ~I2C_CR_I2C_SUB_ADDR_EN;
    }

    regval &= ~I2C_CR_I2C_SLV_ADDR_MASK;
    regval |= (slaveaddr << I2C_CR_I2C_SLV_ADDR_SHIFT);
#if !defined(BL602) && !defined(BL702)
    if (is_addr_10bit) {
        regval |= I2C_CR_I2C_10B_ADDR_EN;
    } else {
        regval &= ~I2C_CR_I2C_10B_ADDR_EN;
    }
#endif
    putreg32(subaddr, reg_base + I2C_SUB_ADDR_OFFSET);
    putreg32(regval, reg_base + I2C_CONFIG_OFFSET);
}

static inline void bflb_i2c_set_datalen(struct bflb_device_s *dev, uint16_t data_len)
{
    uint32_t regval;
    uint32_t reg_base;

    reg_base = dev->reg_base;

    regval = getreg32(reg_base + I2C_CONFIG_OFFSET);
    regval &= ~I2C_CR_I2C_PKT_LEN_MASK;
    regval |= ((data_len - 1) << I2C_CR_I2C_PKT_LEN_SHIFT) & I2C_CR_I2C_PKT_LEN_MASK;
    putreg32(regval, reg_base + I2C_CONFIG_OFFSET);
}


static inline void bflb_i2c_set_dir(struct bflb_device_s *dev, bool is_in)
{
    uint32_t regval;
    uint32_t reg_base;

    reg_base = dev->reg_base;

    regval = getreg32(reg_base + I2C_CONFIG_OFFSET);

    if (is_in) {
        regval |= I2C_CR_I2C_PKT_DIR;
    } else {
        regval &= ~I2C_CR_I2C_PKT_DIR;
    }
    putreg32(regval, reg_base + I2C_CONFIG_OFFSET);
}


static int bflb_i2c_write_bytes(struct bflb_device_s *dev, uint8_t *data, uint32_t len, uint32_t timeout)
{
    uint32_t reg_base;
    uint32_t temp = 0;
    uint8_t *tmp_buf;
    uint64_t start_time;

    reg_base = dev->reg_base;
    tmp_buf = data;
    while (len >= 4) {
        for (uint8_t i = 0; i < 4; i++) {
            temp += (tmp_buf[i] << ((i % 4) * 8));
        }
        tmp_buf += 4;
        len -= 4;
        start_time = bflb_mtimer_get_time_ms();
        while ((getreg32(reg_base + I2C_FIFO_CONFIG_1_OFFSET) & I2C_TX_FIFO_CNT_MASK) == 0) {
            if ((bflb_mtimer_get_time_ms() - start_time) > timeout) {
                return -ETIMEDOUT;
            }
        }
        putreg32(temp, reg_base + I2C_FIFO_WDATA_OFFSET);
        if (!bflb_i2c_isenable(dev)) {
            bflb_i2c_enable(dev);
        }
        temp = 0;
    }

    if (len > 0) {
        for (uint8_t i = 0; i < len; i++) {
            temp += (tmp_buf[i] << ((i % 4) * 8));
        }
        start_time = bflb_mtimer_get_time_ms();
        while ((getreg32(reg_base + I2C_FIFO_CONFIG_1_OFFSET) & I2C_TX_FIFO_CNT_MASK) == 0) {
            if ((bflb_mtimer_get_time_ms() - start_time) > timeout) {
                return -ETIMEDOUT;
            }
        }
        putreg32(temp, reg_base + I2C_FIFO_WDATA_OFFSET);
        if (!bflb_i2c_isenable(dev)) {
            bflb_i2c_enable(dev);
        }
    }

    start_time = bflb_mtimer_get_time_ms();
    while (bflb_i2c_isbusy(dev) || !bflb_i2c_isend(dev) || bflb_i2c_isnak(dev)) {
        if ((bflb_mtimer_get_time_ms() - start_time) > timeout) {
            return -ETIMEDOUT;
        }
    }
    bflb_i2c_disable(dev);

    return 0;
}


static int bflb_i2c_read_bytes(struct bflb_device_s *dev, uint8_t *data, uint32_t len, uint32_t timeout)
{
    uint32_t reg_base;
    uint32_t temp = 0;
    uint8_t *tmp_buf;
    uint64_t start_time;

    reg_base = dev->reg_base;
    tmp_buf = data;

    bflb_i2c_enable(dev);

    while (len >= 4) {
        start_time = bflb_mtimer_get_time_ms();
        while ((getreg32(reg_base + I2C_FIFO_CONFIG_1_OFFSET) & I2C_RX_FIFO_CNT_MASK) == 0) {
            if ((bflb_mtimer_get_time_ms() - start_time) > timeout) {
                return -ETIMEDOUT;
            }
        }
        temp = getreg32(reg_base + I2C_FIFO_RDATA_OFFSET);
        PUT_UINT32_LE(tmp_buf, temp);
        tmp_buf += 4;
        len -= 4;
    }

    if (len > 0) {
        start_time = bflb_mtimer_get_time_ms();
        while ((getreg32(reg_base + I2C_FIFO_CONFIG_1_OFFSET) & I2C_RX_FIFO_CNT_MASK) == 0) {
            if ((bflb_mtimer_get_time_ms() - start_time) > timeout) {
                return -ETIMEDOUT;
            }
        }
        temp = getreg32(reg_base + I2C_FIFO_RDATA_OFFSET);

        for (uint8_t i = 0; i < len; i++) {
            tmp_buf[i] = (temp >> (i * 8)) & 0xff;
        }
    }

    start_time = bflb_mtimer_get_time_ms();
    while (bflb_i2c_isbusy(dev) || !bflb_i2c_isend(dev)) {
        if ((bflb_mtimer_get_time_ms() - start_time) > timeout) {
            return -ETIMEDOUT;
        }
    }
    bflb_i2c_disable(dev);

    return 0;
}



/*
 * address: the 7-bit slave address (optional); if not specified, join the bus as a controller device.
 */
void Wire::begin(unsigned char addr) {
    wire_timeout = 100;
    wire_timeout_flag = false;
    board_i2c_pinmux_init();
    i2c0 = bflb_device_get_by_name("i2c0");
    bflb_i2c_init(i2c0, 50000);
}

void Wire::begin() {
    wire_timeout = 100;
    wire_timeout_flag = false;
    board_i2c_pinmux_init();
    i2c0 = bflb_device_get_by_name("i2c0");
    bflb_i2c_init(i2c0, 50000);
}

void Wire::end() {
    bflb_i2c_deinit(i2c0);
}
/* 
 * address: the 7-bit slave address of the device to request bytes from.
 *
 * quantity: the number of bytes to request.
 *
 * stop: true or false. true will send a stop message after the request, releasing the bus. 
 * False will continually send a restart after the request, keeping the connection active.
 */

int Wire::requestFrom(unsigned char addr, int quantity, bool stop) {
    index = 0;
    bflb_i2c_disable(i2c0);
    bflb_i2c_enable(i2c0);
    bflb_i2c_addr_config(i2c0, addr, 0, 0, false);
    bflb_i2c_set_datalen(i2c0,quantity);
    bflb_i2c_set_dir(i2c0, 1);
    bflb_i2c_read_bytes(i2c0, rbuf,quantity,wire_timeout);
    available_count = quantity;
    if(true == stop){
        bflb_i2c_disable(i2c0);
    }
    return 0;
}

int Wire::requestFrom(unsigned char addr, int quantity) {
    index = 0;
    bflb_i2c_disable(i2c0);
    bflb_i2c_enable(i2c0);
    bflb_i2c_addr_config(i2c0, addr, 0, 0, false);
    bflb_i2c_set_datalen(i2c0,quantity);
    bflb_i2c_set_dir(i2c0, 1);
    bflb_i2c_read_bytes(i2c0, rbuf,quantity,wire_timeout);
    available_count = quantity;
    return 0;
}

/*
 * address: the 7-bit address of the device to transmit to.
 */
void Wire::beginTransmission(unsigned char addr) {
    //bflb_i2c_enable(i2c0);
    bflb_i2c_addr_config(i2c0, addr, 0, 0, false);
    bflb_i2c_set_dir(i2c0, 0);
}

/*
 * stop: true or false. True will send a stop message, releasing the bus after transmission. 
 * False will send a restart, keeping the connection active.
 *
 * Returns
 * 0: success.
 * 1: data too long to fit in transmit buffer.
 * 2: received NACK on transmit of address.
 * 3: received NACK on transmit of data.
 * 4: other error.
 * 5: timeout
 */

void Wire::endTransmission(bool stop) {
    bflb_i2c_disable(i2c0);
}

void Wire::endTransmission() {
    bflb_i2c_disable(i2c0);
    
}

/*
 * Description
 * This function writes data from a peripheral device in response to a request from 
 * a controller device, or queues bytes for transmission from a controller to 
 * peripheral device (in-between calls to beginTransmission() and endTransmission()).
 * Syntax
 * Wire.write(value) Wire.write(string) Wire.write(data, length)
 * Parameters
 * value: a value to send as a single byte.
 * string: a string to send as a series of bytes.
 * data: an array of data to send as bytes.
 * length: the number of bytes to transmit.
 * Returns
 * The number of bytes written (reading this number is optional).
 */
int Wire::write(unsigned char value) {
    bflb_i2c_set_datalen(i2c0, 1); 
    bflb_i2c_write_bytes(i2c0, &value, 1,wire_timeout);
  return 0;
}
int Wire::write(uint8_t *str) {
    bflb_i2c_set_datalen(i2c0, strlen((const char*)str));
    bflb_i2c_write_bytes(i2c0, str, strlen((const char*)str),wire_timeout);
  return 0;
}
int Wire::write(uint8_t *str, int len) {
    bflb_i2c_set_datalen(i2c0, len);
    int ret = bflb_i2c_write_bytes(i2c0, str, len,wire_timeout);
  return ret;
}

/* 
 * Description
 * This function returns the number of bytes available for retrieval with read(). 
 * This function should be called on a controller device after a call to 
 * requestFrom() or on a peripheral inside the onReceive() handler. 
 * available() inherits from the Stream utility class.
 */

int Wire::available() {
    return available_count;
}

/*
 * Description
 * This function reads a byte that was transmitted from a peripheral device to 
 * a controller device after a call to requestFrom() or was transmitted from a 
 * controller device to a peripheral device. read() inherits from the Stream utility class.
 * Syntax
 * Wire.read()
 * Parameters
 * None.
 * Returns
 * The next byte received.
 */

int Wire::read() {
    unsigned char ret;
    if(available_count){
        available_count--;
        ret = rbuf[index];
        index++;
        return ret;
    }
    return 0;
}

/*
 * Description
 * This function modifies the clock frequency for I2C communication. 
 * I2C peripheral devices have no minimum working clock frequency, 
 * however 100KHz is usually the baseline.
 * Syntax
 * Wire.setClock(clockFrequency)
 * Parameters
 * clockFrequency: the value (in Hertz) of the desired communication clock. 
 * Accepted values are 100000 (standard mode) and 400000 (fast mode). 
 * Some processors also support 10000 (low speed mode), 1000000 (fast mode plus) 
 * and 3400000 (high speed mode). Please refer to the specific processor documentation 
 * to make sure the desired mode is supported.
 * Returns
 * None.
 */
void Wire::setClock(int clockFrequency) {
    bflb_i2c_deinit(i2c0);
    bflb_i2c_init(i2c0, clockFrequency);
}

/*
 * Description
 * This function registers a function to be called when a peripheral device receives 
 * a transmission from a controller device.
 * Syntax
 * Wire.onReceive(handler)
 * Parameters
 * handler: the function to be called when the peripheral device receives data; 
 * this should take a single int parameter (the number of bytes read from the controller 
 * device) and return nothing.
 * Returns
 * None.
 */
void Wire::onReceive(void (*callback)(int)) {
    //we not support slave mode yet
}

/*
 * Description
 * This function registers a function to be called when a controller device requests data from a peripheral device.
 * Syntax
 * Wire.onRequest(handler)
 * Parameters
 * handler: the function to be called, takes no parameters and returns nothing.
 * Returns
 * None.
 */
void Wire::onRequest(void (*callback)()) {
    //we not support slave mode yet
}

/* 
 * Description
 * Sets the timeout for Wire transmissions in master mode.
 * Syntax
 * Wire.setWireTimeout(timeout, reset_on_timeout)
 * Wire.setWireTimeout()
 * Parameters
 * timeout a timeout: timeout in microseconds, if zero then timeout checking is disabled
 * reset_on_timeout: if true then Wire hardware will be automatically reset on timeout
 * When this function is called without parameters, a default timeout is configured that 
 * should be sufficient to prevent lockups in a typical single-master configuration.
 * Returns
 * None.
 */
void Wire::setWireTimeout(int timeout, bool reset_on_timeout) {
    wire_timeout = timeout;
    wire_timeout_flag = true;
}

/* Description
 * Clears the timeout flag.
 * Timeouts might not be enabled by default. See the documentation for Wire.setWireTimeout() 
 * for more information on how to configure timeouts and how they work.
 * Syntax
 * Wire.clearTimeout()
 * Parameters
 * None.
 * Returns
 * bool: The current value of the flag
 */
bool Wire::clearWireTimeoutFlag() {
    wire_timeout_flag = false;
  return true;
}

/*
 * Description
 * Checks whether a timeout has occured since the last time the flag was cleared.
 * This flag is set is set whenever a timeout occurs and cleared when Wire.clearWireTimeoutFlag() 
 * is called, or when the timeout is changed using Wire.setWireTimeout().
 * Syntax
 * Wire.getWireTimeoutFlag()
 * Parameters
 * None.
 * Returns
 * bool: The current value of the flag
 */

bool Wire::getWireTimeoutFlag() {
  return wire_timeout_flag;
}



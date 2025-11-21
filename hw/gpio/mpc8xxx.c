/*
 *  GPIO Controller for a lot of Freescale SoCs
 *
 * Copyright (C) 2014 Freescale Semiconductor, Inc. All rights reserved.
 *
 * Author: Alexander Graf, <agraf@suse.de>
 *
 * The code was modified in 03.2020 by Wojciech M. Zabolotny <wzab01@gmail.com>
 * To enable communication with GUI on host machine via POSIX IPC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include <mqueue.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h> 
#include "qemu/main-loop.h"

 //TODO add proper error handling for mq functions and message creation

#define TYPE_MPC8XXX_GPIO "mpc8xxx_gpio"
OBJECT_DECLARE_SIMPLE_TYPE(MPC8XXXGPIOState, MPC8XXX_GPIO)
#define MSG_MAX 8192
typedef struct {
      uint8_t magick[2];
      uint32_t pin;
      uint32_t state;
} gpio_msg;

#define REMOTE_GPIO_MAGICK (0x6910)

struct MPC8XXXGPIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq out[32];

    mqd_t mq_from_qemu;
    mqd_t mq_to_qemu;
    QemuThread thread;
    QemuMutex dat_lock;
    uint32_t dir;
    uint32_t odr;
    uint32_t dat;
    uint32_t ier;
    uint32_t imr;
    uint32_t icr;
};

static const VMStateDescription vmstate_mpc8xxx_gpio = {
    .name = "mpc8xxx_gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dir, MPC8XXXGPIOState),
        VMSTATE_UINT32(odr, MPC8XXXGPIOState),
        VMSTATE_UINT32(dat, MPC8XXXGPIOState),
        VMSTATE_UINT32(ier, MPC8XXXGPIOState),
        VMSTATE_UINT32(imr, MPC8XXXGPIOState),
        VMSTATE_UINT32(icr, MPC8XXXGPIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void mpc8xxx_gpio_update(MPC8XXXGPIOState *s)
{
             gpio_msg msg;
                msg.magick[0] = 0x12;
                msg.magick[1] = 0x34;
                msg.pin = s->ier;
                msg.state = s->imr;
                /* Send the new value */
                mq_send(s->mq_from_qemu,(const char *)&msg,sizeof(msg),0);
                msg.magick[0] = 0x56;
                msg.magick[1] = 0x78;
                msg.pin = !!(s->ier & s->imr);
                msg.state = 0;
                /* Send the new value */
                mq_send(s->mq_from_qemu,(const char *)&msg,sizeof(msg),0);
    qemu_set_irq(s->irq, !!(s->ier & s->imr));
}

static uint64_t mpc8xxx_gpio_read(void *opaque, hwaddr offset,
                                  unsigned size)
{
    MPC8XXXGPIOState *s = (MPC8XXXGPIOState *)opaque;

    if (size != 4) {
        /* All registers are 32bit */
        return 0;
    }

    switch (offset) {
    case 0x0: /* Direction */
        return s->dir;
    case 0x4: /* Open Drain */
        return s->odr;
    case 0x8: /* Data */
        return s->dat;
    case 0xC: /* Interrupt Event */
        return s->ier;
    case 0x10: /* Interrupt Mask */
        return s->imr;
    case 0x14: /* Interrupt Control */
        return s->icr;
    default:
        return 0;
    }
}

static void mpc8xxx_write_data(MPC8XXXGPIOState *s, uint32_t new_data)
{
    uint32_t old_data = s->dat;
    uint32_t diff = old_data ^ new_data;
    int i;

    gpio_msg msg;
    msg.magick[0] = 0x69;
    msg.magick[1] = 0x10;

    qemu_mutex_lock(&s->dat_lock);
    for (i = 0; i < 32; i++) {
        uint32_t mask = 0x80000000 >> i;
        if (!(diff & mask)) {
            continue;
        }

        if (s->dir & mask) {

            msg.pin = i;
            msg.state = (diff & mask) ? 1 : 0;

            /* Output */
            qemu_set_irq(s->out[i], (new_data & mask) != 0);

            /* Send the new value */
            mq_send(s->mq_from_qemu,(const char *)&msg,sizeof(msg),0);
        }
    }

    s->dat = new_data;
    qemu_mutex_unlock(&s->dat_lock);
}

static void mpc8xxx_gpio_write(void *opaque, hwaddr offset,
                        uint64_t value, unsigned size)
{
    MPC8XXXGPIOState *s = (MPC8XXXGPIOState *)opaque;

    if (size != 4) {
        /* All registers are 32bit */
        return;
    }

    switch (offset) {
    case 0x0: /* Direction */
        s->dir = value;
        break;
    case 0x4: /* Open Drain */
        s->odr = value;
        break;
    case 0x8: /* Data */
        mpc8xxx_write_data(s, value);
        break;
    case 0xC: /* Interrupt Event */
        s->ier &= ~value;
        break;
    case 0x10: /* Interrupt Mask */
        s->imr = value;
        break;
    case 0x14: /* Interrupt Control */
        s->icr = value;
        break;
    }

    mpc8xxx_gpio_update(s);
}

static void mpc8xxx_gpio_reset(DeviceState *dev)
{
    MPC8XXXGPIOState *s = MPC8XXX_GPIO(dev);

    s->dir = 0;
    s->odr = 0;
    s->dat = 0;
    s->ier = 0;
    s->imr = 0;
    s->icr = 0;
}

static void mpc8xxx_gpio_set_irq(void * opaque, int irq, int level)
{
    MPC8XXXGPIOState *s = (MPC8XXXGPIOState *)opaque;
    uint32_t mask;

    mask = 0x80000000 >> irq;
    if ((s->dir & mask) == 0) {
        qemu_mutex_lock(&s->dat_lock);
        uint32_t old_value = s->dat & mask;

        s->dat &= ~mask;
        if (level)
            s->dat |= mask;

        // TODO corriger s->icr & irq car c'est un bitmak (icr) et pas un index (irq)
        if (!(s->icr & irq) || (old_value && !level)) {
            s->ier |= mask;
        }

        qemu_mutex_unlock(&s->dat_lock);
        mpc8xxx_gpio_update(s);
    }
}

static const MemoryRegionOps mpc8xxx_gpio_ops = {
    .read = mpc8xxx_gpio_read,
    .write = mpc8xxx_gpio_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void * remote_gpio_thread(void * arg)
{
    MPC8XXXGPIOState *s = (MPC8XXXGPIOState *)arg;
    //Here we receive the data from the queue
    char buf[MSG_MAX];
    gpio_msg * mg = (gpio_msg *)&buf;
 
    while(1) {
        int res = mq_receive(s->mq_to_qemu,buf,MSG_MAX,NULL);
        if(res<0) {
            perror("I can't receive");
            exit(1);
        }
        if(res != sizeof(gpio_msg)) continue;
        if((int) mg->magick[0]*256+mg->magick[1] != REMOTE_GPIO_MAGICK) {
            printf("Wrong message received");
        }
        if(mg->pin < 32) {
            mq_send(s->mq_from_qemu,(const char *)mg,sizeof(gpio_msg),0);
            bql_lock();
            mpc8xxx_gpio_set_irq(arg,mg->pin,mg->state);
            bql_unlock();
        } else if(mg->pin == 255) {
            //This is a special "reconnect message" it enforces sending state of all pins
            int i;
            uint32_t dat = s->dat;
            uint32_t mask = 0x80000000;
            for(i=0;i<32;i++) {
                gpio_msg msg;
                msg.magick[0] = 0x69;
                msg.magick[1] = 0x10;
                msg.pin = i;
                msg.state = (dat & mask) ? 1 : 0;
                /* Send the new value */
                mq_send(s->mq_from_qemu,(const char *)&msg,sizeof(msg),0);
                /* Update the bit in the dat field */
                mask >>= 1;
            }
        }
    }
}

static void mpc8xxx_gpio_initfn(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    MPC8XXXGPIOState *s = MPC8XXX_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &mpc8xxx_gpio_ops,
                          s, "mpc8xxx_gpio", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_in(dev, mpc8xxx_gpio_set_irq, 32);
    qdev_init_gpio_out(dev, s->out, 32);

    qemu_mutex_init(&s->dat_lock);
    mqd_t mq = mq_open("/from_qemu",O_CREAT | O_WRONLY,S_IRUSR | S_IWUSR,NULL);

    if(mq<0) {
        perror("I can't open mq");
        exit(1);
    }
    s->mq_from_qemu = mq;

    mq = mq_open("/to_qemu",O_CREAT | O_RDONLY,S_IRUSR | S_IWUSR,NULL);
    if(mq<0) {
        perror("I can't open mq");
        exit(1);
    }
    s->mq_to_qemu = mq;

    qemu_thread_create(&s->thread, "remote_gpio", remote_gpio_thread, s,
                       QEMU_THREAD_JOINABLE);
}

static void mpc8xxx_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_mpc8xxx_gpio;
    device_class_set_legacy_reset(dc, mpc8xxx_gpio_reset);
}

static const TypeInfo mpc8xxx_gpio_types[] = {
    {
        .name          = TYPE_MPC8XXX_GPIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MPC8XXXGPIOState),
        .instance_init = mpc8xxx_gpio_initfn,
        .class_init    = mpc8xxx_gpio_class_init,
    },
};

DEFINE_TYPES(mpc8xxx_gpio_types)

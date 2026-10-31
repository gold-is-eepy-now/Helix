# KVM Driver Makefile

# Optional KVM/virtio driver support
# This is part of the optional drivers library

KVM_DRIVER_DIR := $(CURDIR)
OBJDIR := $(KVM_DRIVER_DIR)/build

# Source files
SRCS := $(KVM_DRIVER_DIR)/kvm_driver.c

# Object files
OBJS := $(SRCS:.c=.o)
OBJS := $(addprefix $(OBJDIR)/,$(OBJS))

# Target library
LIB := $(KVM_DRIVER_DIR)/build/kvm_driver.a

.PHONY: all clean

all: $(LIB)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(KVM_DRIVER_DIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(KVM_DRIVER_DIR) -c $< -o $@

$(LIB): $(OBJS)
	@mkdir -p $(OBJDIR)
	ar rcs $@ $(OBJS)

clean:
	rm -rf $(OBJDIR) $(LIB)
# Makefile for RIST Metrics Collector

CC = gcc
CFLAGS = -Wall -Wextra -O3 -pthread -std=c11
LIBS = -lcurl -lhiredis -lm
TARGET = metrics-collector

# Installation paths
INSTALL_BIN = /usr/local/bin
INSTALL_SERVICE = /etc/systemd/system

all: $(TARGET)

$(TARGET): metrics-collector.c
	@echo "Compiling $(TARGET)..."
	$(CC) $(CFLAGS) -o $(TARGET) metrics-collector.c $(LIBS)
	@echo "Build complete!"

clean:
	@echo "Cleaning..."
	rm -f $(TARGET)

install: $(TARGET)
	@echo "Installing $(TARGET)..."
	sudo cp $(TARGET) $(INSTALL_BIN)/
	sudo chmod +x $(INSTALL_BIN)/$(TARGET)
	@echo "Installing systemd service..."
	sudo cp metrics-collector.service $(INSTALL_SERVICE)/
	sudo systemctl daemon-reload
	@echo ""
	@echo "Installation complete!"
	@echo "To start the service:"
	@echo "  sudo systemctl enable metrics-collector"
	@echo "  sudo systemctl start metrics-collector"
	@echo ""
	@echo "To check status:"
	@echo "  sudo systemctl status metrics-collector"
	@echo ""
	@echo "To view logs:"
	@echo "  sudo journalctl -u metrics-collector -f"

uninstall:
	@echo "Stopping service..."
	-sudo systemctl stop metrics-collector
	-sudo systemctl disable metrics-collector
	@echo "Removing files..."
	sudo rm -f $(INSTALL_BIN)/$(TARGET)
	sudo rm -f $(INSTALL_SERVICE)/metrics-collector.service
	sudo systemctl daemon-reload
	@echo "Uninstall complete!"

test: $(TARGET)
	@echo "Running metrics collector in foreground..."
	./$(TARGET)

deps:
	@echo "Installing dependencies..."
	@echo "Detected OS:"
	@cat /etc/os-release | grep PRETTY_NAME
	@echo ""
	@echo "Installing build tools and libraries..."
	sudo apt-get update
	sudo apt-get install -y build-essential libcurl4-openssl-dev libhiredis-dev redis-server
	@echo ""
	@echo "Dependencies installed!"
	@echo ""
	@echo "Note: If you need Redis TimeSeries module, install it with:"
	@echo "  sudo apt-get install redis-timeseries"
	@echo "  or use Docker:"
	@echo "  docker run -d -p 6379:6379 redis/redis-stack-server:latest"

.PHONY: all clean install uninstall test deps

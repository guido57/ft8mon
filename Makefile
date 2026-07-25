CXX = c++ -O -fstack-usage
# CXX += -g -fsanitize=address
# CXX = g++9 -O3
FLAGS = -std=c++17 -I/opt/local/include -I/usr/local/include 
LIBS = -L/opt/local/lib -L/usr/local/lib

MOREC = 
MOREH = 


ft8mon: ft8.cc kiss_fft.c kiss_fftr.c ft8mon.cc libldpc.c osd.cc unpack.cc util.cc fft.cc  $(MOREC) $(MOREH)
	$(CXX) $(FLAGS) ft8mon.cc ft8.cc kiss_fft.c kiss_fftr.c unpack.cc osd.cc util.cc fft.cc libldpc.c  $(MOREC) -o ft8mon $(LIBS) 

clean:
	rm -f ft8mon

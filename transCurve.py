import matplotlib.pyplot as plt

# Read a file such as "queue-0-0.dat".
def readData(filename):
    x = []
    y = []
    with open(filename, "r") as f:
        while True:
            line = f.readline()
            if line=="":
                break
            cols = line.split()
            time = float(cols[0])
            num = int(cols[4])
            x.append(time)
            y.append(num)
    return x,y

def compare(file1, file2, name1, name2):
    x1, y1 = readData(file1)
    plt.plot(x1, y1)
    x2, y2 = readData(file2)
    plt.plot(x2, y2)
    plt.legend([name1, name2])
    plt.xlabel("Time(seconds)")
    plt.ylabel("Throughput(packets)")
    plt.show()


compare("results/tcd/example_congest/queue-6-2.dat", "results/tcd/example_congest/queue-5-2.dat", "6-2", "5-2")

import sys
import os
import subprocess
import random
from collections import defaultdict
import time

if len(sys.argv) != 3:
    print("Usage: [project] [num_times]")
    exit(-1)

num_times = int(sys.argv[2])
seen = set()
os.chdir(sys.argv[1])
for _ in range(num_times):
    print("Starting a cycle")

    cur = None
    if not os.path.isfile('../grade'):
        cur = 1
    else:
        with open('../grade', 'r') as temp_file:
            for line in temp_file:
                if 'Total' in line and 'Tests' not in line:
                    temp = line.replace(" ", "").replace("%", "").replace("Total", "").split("/")
                    score1 = float(temp[0])
                    score2 = float(temp[1])
                    cur = score1 / score2

    if not seen and os.path.isfile('../stress_log'):
        with open('../stress_log', 'r') as temp:
            for line in temp:
                splitted = line.replace('Run with Jitter ', '').split(':')
                seen.add(int(splitted[0]))

    log = open('../stress_log', 'a')

    jitter_value = random.randint(1, 1000)
    if len(seen) / (1000) >= .75:
        print("Stress log near full of jitter values, please clear")
    else:
        while jitter_value in seen:
            jitter_value = random.randint(1, 1000)
        
    seen.add(jitter_value)
    jitter = "PINTOSOPTS=-j " + str(jitter_value)

    subprocess.run(['make', 'clean'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['make', '-j8'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['make', 'grade', 'SIMULATOR=--bochs', jitter], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    lower = False
    with open('build/grade', 'r') as new_grade_file:
        for line in new_grade_file:
            if 'Total' in line and 'Tests' not in line and '%/' in line:
                temp = line.replace(" ", "").replace("%", "").replace("Total", "").split("/")
                score1 = float(temp[0])
                score2 = float(temp[1])
                score = score1 / score2
                if score <= cur:
                    cur = score
                    lower = True

                log.write("Run with Jitter " + str(jitter_value) + ": " + str(score) + "\n")
                break

    log.close()

    if lower:
        with open('../grade', 'w') as replaced_grade_file, open('build/grade', 'r') as new_grade_file:
            first_line = "Jitter Value: " + str(jitter_value) + "\n"
            replaced_grade_file.write(first_line)
            for line in new_grade_file:
                replaced_grade_file.write(line)
    
    subprocess.run(['rm', '-rf', 'build/grade'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print("Finished one cycle")
    print()

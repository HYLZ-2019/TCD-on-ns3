import json

nodes = {
    '0': 'node-0',
    '1': 'node-1'
}

# (from, to)
edges =[
    ('0-0', '1-0')
]

json_content = {
    "nodes": [],
    "edges": []
}

for node in nodes.keys():
    json_content["nodes"].append({
        "id": nodes[node]
    })  

for edge in edges:
    edge_dict = {
        "source": nodes[edge[0].split('-')[0]],
        "target": nodes[edge[1].split('-')[0]],
        "sTimeline": [],
        "tTimeline": []
    }
    for i in [0, 1]:
        with open(f"results/queue-{edge[i]}.dat") as f:
            rows = f.readlines()
            for row in rows:
                data = row.split()
                time, queue = data[0], data[1]
                if i == 0:
                    edge_dict['sTimeline'].append((time, queue))
                else:
                    edge_dict['tTimeline'].append((time, queue))
    json_content['edges'].append(edge_dict)

json_str = json.dumps(json_content)

with open("results/data.json", 'w') as f:
    f.write(json_str)


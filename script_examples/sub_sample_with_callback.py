import pycc
import numpy as np

CC = pycc.GetInstance()


def main():
    entities = CC.getSelectedEntities()

    if not entities:
        raise RuntimeError("No entities selected")

    pc = entities[0]
    print(pc, pc.size())

    progress = pycc.ccProgressDialog()
    progress.start()
    refcloud = pycc.RunInThread(pycc.CloudSamplingTools.subsampleCloudRandomly, pc, pc.size() // 2, progressCb=progress)
    randomPc = pc.partialClone(refcloud, CC)
    randomPc.setName("Randomly subsampled")

    refcloud.addPointIndex(0)
    refcloud.addPointIndex(0, 10)


if __name__ == '__main__':
    main()

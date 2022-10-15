
#include "common.hpp"
#include "render.hpp"
#include "fp32vec4.hpp"

#include <algorithm>

bool Renderer::RenderObject::operator<(const RenderObject& r) const
{
  if ((flags & 2) != (r.flags & 2))
    return bool(r.flags & 2);
  if (flags & 2)
  {
    unsigned int  modelID1 = model.o->modelID;
    unsigned int  modelID2 = r.model.o->modelID;
    unsigned int  modelMask1 = ~(Renderer::modelBatchCnt - 1U);
    if ((modelID1 & modelMask1) != (modelID2 & modelMask1))
      return ((modelID1 & modelMask1) < (modelID2 & modelMask1));
    if (tileIndex != r.tileIndex)
      return (tileIndex < r.tileIndex);
    if (z != r.z)
      return (z < r.z);
    unsigned int  modelMask2 = Renderer::modelBatchCnt - 1U;
    return ((modelID1 & modelMask2) < (modelID2 & modelMask2));
  }
  if (tileIndex != r.tileIndex)
    return (tileIndex < r.tileIndex);
  if (z != r.z)
    return (z < r.z);
  if (model.t.y0 != r.model.t.y0)
    return (model.t.y0 < r.model.t.y0);
  return (model.t.x0 < r.model.t.x0);
}

Renderer::ModelData::ModelData()
  : nifFile((NIFFile *) 0),
    totalTriangleCnt(0),
    o((BaseObject *) 0),
    loadThread((std::thread *) 0)
{
}

Renderer::ModelData::~ModelData()
{
  if (loadThread)
  {
    loadThread->join();
    delete loadThread;
    loadThread = (std::thread *) 0;
  }
  clear();
}

void Renderer::ModelData::clear()
{
  if (nifFile)
  {
    delete nifFile;
    nifFile = (NIFFile *) 0;
  }
  totalTriangleCnt = 0;
  meshData.clear();
  o = (BaseObject *) 0;
  threadErrMsg.clear();
  fileBuf.clear();
}

Renderer::RenderThread::RenderThread()
  : t((std::thread *) 0),
    terrainMesh((TerrainMesh *) 0),
    renderer((Plot3D_TriShape *) 0)
{
}

Renderer::RenderThread::~RenderThread()
{
  if (renderer)
  {
    delete renderer;
    renderer = (Plot3D_TriShape *) 0;
  }
  clear();
}

void Renderer::RenderThread::join()
{
  if (t)
  {
    t->join();
    delete t;
    t = (std::thread *) 0;
  }
}

void Renderer::RenderThread::clear()
{
  join();
  errMsg.clear();
  if (terrainMesh)
  {
    delete terrainMesh;
    terrainMesh = (TerrainMesh *) 0;
  }
  objectsRemaining.clear();
}

unsigned long long Renderer::calculateTileMask(int x0, int y0,
                                               int x1, int y1) const
{
  unsigned int  xMask = 0U;
  for (int i = 0; i < 8; i++)
  {
    if (x0 < ((width * ((i & 7) + 1)) >> 3) && x1 >= ((width * (i & 7)) >> 3))
      xMask = xMask | (1U << (unsigned int) i);
  }
  unsigned long long  m = 0ULL;
  for (int i = 0; i < 8; i++)
  {
    if (y0 < ((height * ((i & 7) + 1)) >> 3) && y1 >= ((height * (i & 7)) >> 3))
      m = m | ((unsigned long long) xMask << (unsigned int) (i << 3));
  }
  return m;
}

signed short Renderer::calculateTileIndex(unsigned long long screenAreasUsed)
{
  if (!screenAreasUsed)
    return -1;
  if (!(screenAreasUsed & (screenAreasUsed - 1ULL)))
  {
    unsigned int  n0 = 0;
    unsigned int  n2 = 64;
    while ((1ULL << n0) < screenAreasUsed)
    {
      unsigned int  n1 = (n0 + n2) >> 1;
      if ((1ULL << n1) > screenAreasUsed)
        n2 = n1;
      else
        n0 = n1;
    }
    return (signed short) n0;
  }
  unsigned long long  m = screenAreasUsed;
  int     x0 = 7;
  int     x1 = 0;
  for (int x = 0; m; x++, m = ((m >> 1) & 0x7F7F7F7F7F7F7F7FULL))
  {
    if (m & 0x0101010101010101ULL)
    {
      x0 = (x < x0 ? x : x0);
      x1 = x;
    }
  }
  m = screenAreasUsed;
  int     y0 = 7;
  int     y1 = 0;
  for (int y = 0; m; y++, m = ((m >> 8) & 0x00FFFFFFFFFFFFFFULL))
  {
    if (m & 0xFFULL)
    {
      y0 = (y < y0 ? y : y0);
      y1 = y;
    }
  }
  if ((x1 - x0) <= 1 && (y1 - y0) <= 1)
  {
    return (signed short) (((y0 & 6) << 3) + (x0 & 6) + 64
                           + ((y0 & 1) << 7) + ((x0 & 1) << 6));
  }
  if ((x1 - x0) <= 3 && (y1 - y0) <= 3)
  {
    return (signed short) (((y0 & 4) << 3) + (x0 & 4) + 320
                           + ((y0 & 3) << 8) + ((x0 & 3) << 6));
  }
  return 1344;
}

int Renderer::setScreenAreaUsed(RenderObject& p)
{
  p.tileIndex = -1;
  NIFFile::NIFVertexTransform vt;
  NIFFile::NIFBounds  modelBounds;
  if (p.flags & 2)
  {
    int     x0 = int(p.model.o->obndX0 < p.model.o->obndX1 ?
                     p.model.o->obndX0 : p.model.o->obndX1) - 2;
    int     x1 =
        x0 + std::abs(int(p.model.o->obndX1) - int(p.model.o->obndX0)) + 4;
    int     y0 = int(p.model.o->obndY0 < p.model.o->obndY1 ?
                     p.model.o->obndY0 : p.model.o->obndY1) - 2;
    int     y1 =
        y0 + std::abs(int(p.model.o->obndY1) - int(p.model.o->obndY0)) + 4;
    int     z0 = int(p.model.o->obndZ0 < p.model.o->obndZ1 ?
                     p.model.o->obndZ0 : p.model.o->obndZ1) - 2;
    int     z1 =
        z0 + std::abs(int(p.model.o->obndZ1) - int(p.model.o->obndZ0)) + 4;
    modelBounds.boundsMin = FloatVector4(float(x0), float(y0), float(z0), 0.0f);
    modelBounds.boundsMax = FloatVector4(float(x1), float(y1), float(z1), 0.0f);
    vt = p.modelTransform;
    vt *= viewTransform;
  }
  else if (p.flags & 1)
  {
    if (!landData)
      return -1;
    float   xyScale = 4096.0f / float(landData->getCellResolution());
    float   xOffset = -xyScale * float(landData->getOriginX());
    float   yOffset = xyScale * float(landData->getOriginY());
    float   zScale = (landData->getZMax() - landData->getZMin()) / 65535.0f;
    float   zOffset = landData->getZMin();
    int     x0 = (p.model.t.x0 < p.model.t.x1 ? p.model.t.x0 : p.model.t.x1);
    int     y0 = (p.model.t.y0 < p.model.t.y1 ? p.model.t.y0 : p.model.t.y1);
    int     x1 = x0 + std::abs(int(p.model.t.x1) - int(p.model.t.x0));
    int     y1 = y0 + std::abs(int(p.model.t.y1) - int(p.model.t.y0));
    modelBounds.boundsMin =
        FloatVector4(float(x0) * xyScale + xOffset,
                     float(y1) * -xyScale + yOffset, 0.0f, 0.0f);
    modelBounds.boundsMax =
        FloatVector4(float(x1) * xyScale + xOffset,
                     float(y0) * -xyScale + yOffset, 0.0f, 0.0f);
    int     w = landData->getImageWidth();
    int     h = landData->getImageHeight();
    x0 = (x0 > 0 ? (x0 < (w - 1) ? x0 : (w - 1)) : 0);
    x1 = (x1 > 0 ? (x1 < (w - 1) ? x1 : (w - 1)) : 0);
    std::uint16_t z0 = 0xFFFF;
    std::uint16_t z1 = 0;
    for (int y = y0; y <= y1; y++)
    {
      int     yc = (y > 0 ? (y < (h - 1) ? y : (h - 1)) : 0);
      const std::uint16_t *srcPtr = landData->getHeightMap();
      srcPtr = srcPtr + (size_t(yc) * size_t(w));
      for (int x = x0; x <= x1; x++)
      {
        std::uint16_t z = srcPtr[x];
        z0 = (z < z0 ? z : z0);
        z1 = (z > z1 ? z : z1);
      }
    }
    modelBounds.boundsMin[2] = float(int(z0)) * zScale + zOffset;
    modelBounds.boundsMax[2] = float(int(z1)) * zScale + zOffset;
    vt = viewTransform;
  }
  else if (p.flags & 4)
  {
    int     x0 = int(p.model.t.x0 < p.model.t.x1 ? p.model.t.x0 : p.model.t.x1);
    int     x1 = x0 + std::abs(int(p.model.t.x1) - int(p.model.t.x0));
    int     y0 = int(p.model.t.y0 < p.model.t.y1 ? p.model.t.y0 : p.model.t.y1);
    int     y1 = y0 + std::abs(int(p.model.t.y1) - int(p.model.t.y0));
    modelBounds.boundsMin = FloatVector4(float(x0), float(y0), 0.0f, 0.0f);
    modelBounds.boundsMax = FloatVector4(float(x1), float(y1), 0.0f, 0.0f);
    vt = p.modelTransform;
    vt *= viewTransform;
  }
  else
  {
    return -1;
  }
  NIFFile::NIFBounds  screenBounds;
  for (int i = 0; i < 8; i++)
  {
    FloatVector4  v((!(i & 1) ? modelBounds.xMin() : modelBounds.xMax()),
                    (!(i & 2) ? modelBounds.yMin() : modelBounds.yMax()),
                    (!(i & 4) ? modelBounds.zMin() : modelBounds.zMax()), 0.0f);
    v = vt.transformXYZ(v);
    screenBounds += v;
  }
  worldBounds += screenBounds.boundsMin;
  worldBounds += screenBounds.boundsMax;
  screenBounds.boundsMin -= 2.0f;
  screenBounds.boundsMax += 2.0f;
  int     xMin = roundFloat(screenBounds.xMin());
  int     yMin = roundFloat(screenBounds.yMin());
  int     zMin = roundFloat(screenBounds.zMin());
  int     xMax = roundFloat(screenBounds.xMax());
  int     yMax = roundFloat(screenBounds.yMax());
  int     zMax = roundFloat(screenBounds.zMax());
  if (xMin >= width || xMax < 0 || yMin >= height || yMax < 0 ||
      zMin >= zRangeMax || zMax < 0)
  {
    return -1;
  }
  p.tileIndex = calculateTileIndex(calculateTileMask(xMin, yMin, xMax, yMax));
  p.z = roundFloat(screenBounds.zMin() * 64.0f);
  return p.tileIndex;
}

unsigned int Renderer::getDefaultWorldID() const
{
  if (landData && landData->getLandTexture16())
    return 0x0025DA15U;
  return 0x0000003CU;
}

void Renderer::addTerrainCell(const ESMFile::ESMRecord& r)
{
  if (!(r == "CELL" && landData))
    return;
  int     cellX = 0;
  int     cellY = 0;
  unsigned int  cellFlags = 0;
  ESMFile::ESMField f(esmFile, r);
  while (f.next())
  {
    if (f == "DATA" && f.size() >= 1)
    {
      cellFlags = f.readUInt8Fast();
    }
    else if (f == "XCLC" && f.size() >= 8)
    {
      cellX = f.readInt32();
      cellY = f.readInt32();
    }
  }
  if (cellFlags & 0x01)                 // interior cell
    return;
  RenderObject  tmp;
  tmp.flags = 0x0001;                   // terrain
  tmp.tileIndex = -1;
  tmp.z = 0;
  tmp.mswpFormID = 0;
  int     w = landData->getImageWidth();
  int     h = landData->getImageHeight();
  int     n = landData->getCellResolution();
  int     x0 = landData->getOriginX() + (cellX * n);
  int     y0 = landData->getOriginY() - ((cellY + 1) * n);
  int     x1 = x0 + n;
  int     y1 = y0 + n;
  if (debugMode == 1)
    n = n >> 1;
  else if (n > 24)
    n = n - 8;
  int     y = y0 - (y0 % n);
  y = (y >= 0 ? y : (y - n));
  for (int i = 0; i < 3; i++, y = y + n)
  {
    int     y2 = y + n;
    if (!(y >= y0 && y < y1 && y < h && y2 > 0))
      continue;
    int     x = x0 - (x0 % n);
    x = (x >= 0 ? x : (x - n));
    for (int j = 0; j < 3; j++, x = x + n)
    {
      int     x2 = x + n;
      if (!(x >= x0 && x < x1 && x < w && x2 > 0))
        continue;
      tmp.model.t.x0 = (signed short) (x > 0 ? x : 0);
      tmp.model.t.y0 = (signed short) (y > 0 ? y : 0);
      tmp.model.t.x1 = (signed short) (x2 < w ? x2 : w);
      tmp.model.t.y1 = (signed short) (y2 < h ? y2 : h);
      if (setScreenAreaUsed(tmp) >= 0)
      {
        if (debugMode == 1)
          tmp.z = int(r.formID);
        objectList.push_back(tmp);
      }
    }
  }
}

void Renderer::addWaterCell(const ESMFile::ESMRecord& r)
{
  if (!waterColor)
    return;
  int     cellX = 0;
  int     cellY = 0;
  unsigned int  cellFlags = 0;
  float   waterLevel = 1.0e12f;
  ESMFile::ESMField f(esmFile, r);
  while (f.next())
  {
    if (f == "DATA" && f.size() >= 1)
    {
      cellFlags = f.readUInt8Fast();
    }
    else if (f == "XCLC" && f.size() >= 8)
    {
      cellX = f.readInt32();
      cellY = f.readInt32();
    }
    else if (f == "XCLW" && f.size() >= 4)
    {
      waterLevel = f.readFloat();
    }
  }
  if (!(cellFlags & 0x02))              // cell has no water
    return;
  if (!(waterLevel >= -1048576.0f && waterLevel <= 1048576.0f))
  {
    if (cellFlags & 0x01)
      return;
    waterLevel = defaultWaterLevel;
  }
  RenderObject  tmp;
  tmp.flags = 0x000C;                   // water cell, alpha blending
  tmp.tileIndex = -1;
  tmp.z = 0;
  tmp.model.t.x0 = 0;
  tmp.model.t.y0 = 0;
  tmp.model.t.x1 = 4096;
  tmp.model.t.y1 = 4096;
  tmp.mswpFormID = 0;
  tmp.modelTransform.offsX = float(cellX) * 4096.0f;
  tmp.modelTransform.offsY = float(cellY) * 4096.0f;
  tmp.modelTransform.offsZ = waterLevel;
  if (setScreenAreaUsed(tmp) >= 0)
  {
    if (debugMode == 1)
      tmp.z = int(r.formID);
    getWaterColor(tmp, r);
    objectList.push_back(tmp);
  }
}

const Renderer::BaseObject * Renderer::readModelProperties(
    RenderObject& p, const ESMFile::ESMRecord& r)
{
  std::map< unsigned int, BaseObject >::const_iterator  i =
      baseObjects.find(r.formID);
  if (i == baseObjects.end())
  {
    BaseObject  tmp;
    tmp.type = 0;
    tmp.flags = 0x0000;
    tmp.modelID = 0xFFFFFFFFU;
    tmp.mswpFormID = 0U;
    tmp.obndX0 = 0;
    tmp.obndY0 = 0;
    tmp.obndZ0 = 0;
    tmp.obndX1 = 0;
    tmp.obndY1 = 0;
    tmp.obndZ1 = 0;
    do
    {
      switch (r.type)
      {
        case 0x49544341:                // "ACTI"
          // ignore markers
          if (r.flags & 0x00800000U)
            continue;
          break;
        case 0x4E525546:                // "FURN"
        case 0x4C4F4353:                // "SCOL"
        case 0x54415453:                // "STAT"
        case 0x45455254:                // "TREE"
          // ignore markers and (optionally) objects not visible from distance
          if ((r.flags | (!distantObjectsOnly ? 0xFF7FFFFFU : 0xFF7F7FFFU))
              != 0xFF7FFFFFU)
          {
            continue;
          }
          break;
        case 0x5454534D:                // "MSTT"
        case 0x54415750:                // "PWAT"
          break;
        default:
          if (!enableAllObjects ||
              (r.flags | (!distantObjectsOnly ? 0xFF7FFFFFU : 0xFF7F7FFFU))
              != 0xFF7FFFFFU)
          {
            continue;
          }
          break;
      }
      bool    haveOBND = false;
      bool    isWater = (r == "PWAT");
      bool    isHDModel = false;
      stringBuf.clear();
      ESMFile::ESMField f(esmFile, r);
      while (f.next())
      {
        if (f == "OBND" && f.size() >= 6)
        {
          tmp.obndX0 = (signed short) uint16ToSigned(f.readUInt16Fast());
          tmp.obndY0 = (signed short) uint16ToSigned(f.readUInt16Fast());
          tmp.obndZ0 = (signed short) uint16ToSigned(f.readUInt16Fast());
          tmp.obndX1 = (signed short) uint16ToSigned(f.readUInt16Fast());
          tmp.obndY1 = (signed short) uint16ToSigned(f.readUInt16Fast());
          tmp.obndZ1 = (signed short) uint16ToSigned(f.readUInt16Fast());
          haveOBND = true;
        }
        else if (f == "MODL" && f.size() > 4 &&
                 (!modelLOD || stringBuf.empty()))
        {
          f.readPath(stringBuf, std::string::npos, "meshes/", ".nif");
          isHDModel = isHighQualityModel(stringBuf);
          if (r == "SCOL" && esmFile.getESMVersion() >= 0xC0)
          {
            if (stringBuf.find(".esm/", 0, 5) == std::string::npos)
            {
              // fix invalid SCOL model paths in SeventySix.esm
              stringBuf = "meshes/scol/seventysix.esm/cm00000000.nif";
              unsigned int  n = r.formID;
              for (int j = 36; n; j--, n = n >> 4)
                stringBuf[j] = char(n & 15U) + ((n & 15U) < 10U ? '0' : 'W');
            }
          }
        }
        else if (f == "MNAM" && f.size() == 1040 && modelLOD && !isHDModel)
        {
          for (int j = (modelLOD - 1) & 3; j >= 0; j--)
          {
            f.setPosition(size_t(j * 260));
            if (f[f.getPosition()] != 0)
            {
              f.readPath(stringBuf, std::string::npos, "meshes/", ".nif");
              if (!stringBuf.empty())
                break;
            }
          }
        }
        else if (f == "MODS" && f.size() >= 4)
        {
          tmp.mswpFormID = f.readUInt32Fast();
        }
        else if (f == "MODC" && f.size() >= 4)
        {
          tmp.flags = (unsigned short) (((roundFloat(f.readFloat() * 255.0f)
                                          & 0xFF) << 8) | 0x80);
        }
        else if (f == "WNAM" && r == "ACTI" && f.size() >= 4)
        {
          isWater = true;
        }
      }
      if (!haveOBND || stringBuf.empty())
        continue;
      if (!isWater)
      {
        if (std::strncmp(stringBuf.c_str(), "meshes/water/", 13) == 0 ||
            stringBuf.find("/fxwaterfall") != std::string::npos)
        {
          isWater = true;
        }
      }
      if ((!isWater && isExcludedModel(stringBuf)) || (isWater && !waterColor))
        continue;
      tmp.type = (unsigned short) (r.type & 0xFFFFU);
      tmp.flags = tmp.flags | (unsigned short) ((!isWater ? 0x02 : 0x06)
                                                | (!isHDModel ? 0x00 : 0x40));
      tmp.modelPath = stringBuf;
    }
    while (false);
    if (tmp.mswpFormID)
    {
      tmp.mswpFormID =
          materialSwaps.loadMaterialSwap(ba2File, esmFile, tmp.mswpFormID);
    }
    i = baseObjects.insert(std::pair< unsigned int, BaseObject >(
                               r.formID, tmp)).first;
  }
  if (!(i->second.flags & 7))
    return (BaseObject *) 0;
  p.flags = i->second.flags;
  p.model.o = &(i->second);
  p.mswpFormID = i->second.mswpFormID;
  return &(i->second);
}

void Renderer::addSCOLObjects(const ESMFile::ESMRecord& r,
                              float scale, float rX, float rY, float rZ,
                              float offsX, float offsY, float offsZ,
                              unsigned int refrMSWPFormID)
{
  NIFFile::NIFVertexTransform vt(scale, rX, rY, rZ, offsX, offsY, offsZ);
  RenderObject  tmp;
  tmp.tileIndex = -1;
  tmp.z = 0;
  ESMFile::ESMField f(esmFile, r);
  unsigned int  mswpFormID_SCOL = 0U;
  unsigned int  mswpFormID_ONAM = 0U;
  const BaseObject  *o = (BaseObject *) 0;
  while (f.next())
  {
    if (f == "MODS" && f.size() >= 4)
    {
      mswpFormID_SCOL = f.readUInt32Fast();
    }
    else if (f == "ONAM" && f.size() >= 4)
    {
      o = (BaseObject *) 0;
      unsigned int  modelFormID = f.readUInt32Fast();
      if (!modelFormID)
        continue;
      mswpFormID_ONAM = 0U;
      if (f.size() >= 8)
        mswpFormID_ONAM = f.readUInt32Fast();
      const ESMFile::ESMRecord  *r3 = esmFile.getRecordPtr(modelFormID);
      if (!r3 || (r3->flags | (!distantObjectsOnly ? 0xFF7FFFFFU : 0xFF7F7FFFU))
                 != 0xFF7FFFFFU)
      {
        // ignore markers and (optionally) objects not visible from distance
        continue;
      }
      if (!(o = readModelProperties(tmp, *r3)) || (tmp.flags & 4))
      {
        o = (BaseObject *) 0;
        continue;
      }
      tmp.mswpFormID = 0U;
    }
    else if (f == "DATA" && o)
    {
      while ((f.getPosition() + 28) <= f.size())
      {
        offsX = f.readFloat();
        offsY = f.readFloat();
        offsZ = f.readFloat();
        rX = f.readFloat();
        rY = f.readFloat();
        rZ = f.readFloat();
        scale = f.readFloat();
        tmp.modelTransform =
            NIFFile::NIFVertexTransform(scale, rX, rY, rZ, offsX, offsY, offsZ);
        tmp.modelTransform *= vt;
        if (setScreenAreaUsed(tmp) >= 0)
        {
          if (!tmp.mswpFormID)
          {
            if (refrMSWPFormID)
              tmp.mswpFormID = refrMSWPFormID;
            else if (mswpFormID_ONAM)
              tmp.mswpFormID = mswpFormID_ONAM;
            else if (mswpFormID_SCOL)
              tmp.mswpFormID = mswpFormID_SCOL;
            else
              tmp.mswpFormID = o->mswpFormID;
            if (tmp.mswpFormID && tmp.mswpFormID != o->mswpFormID)
            {
              tmp.mswpFormID = materialSwaps.loadMaterialSwap(
                                   ba2File, esmFile, tmp.mswpFormID);
            }
          }
          objectList.push_back(tmp);
        }
      }
    }
  }
}

void Renderer::findObjects(unsigned int formID, int type, bool isRecursive)
{
  const ESMFile::ESMRecord  *r = (ESMFile::ESMRecord *) 0;
  do
  {
    r = esmFile.getRecordPtr(formID);
    if (BRANCH_UNLIKELY(!r))
      break;
    if (BRANCH_UNLIKELY(!(*r == "REFR")))
    {
      if (*r == "CELL")
      {
        const ESMFile::ESMRecord  *r2;
        if (!(r->parent && bool(r2 = esmFile.getRecordPtr(r->parent)) &&
              r2->formID == 1U))        // ignore starting cell at 0, 0
        {
          if (type == 0)
            addTerrainCell(*r);
          else
            addWaterCell(*r);
        }
      }
      if (!isRecursive)
        break;
      if (*r == "GRUP" && r->children)
      {
        if (r->formID > 0U && r->formID < (!type ? 6U : 10U) && r->formID != 7U)
          findObjects(r->children, type, true);
      }
      continue;
    }
    if ((r->flags & (!noDisabledObjects ? 0x00000020 : 0x00000820)) || !type)
      continue;                         // ignore deleted and disabled records
    const ESMFile::ESMRecord  *r2 = (ESMFile::ESMRecord *) 0;
    float   scale = 1.0f;
    float   rX = 0.0f;
    float   rY = 0.0f;
    float   rZ = 0.0f;
    float   offsX = 0.0f;
    float   offsY = 0.0f;
    float   offsZ = 0.0f;
    unsigned int  refrMSWPFormID = 0U;
    {
      ESMFile::ESMField f(esmFile, *r);
      while (f.next())
      {
        if (f == "NAME" && f.size() >= 4)
        {
          r2 = esmFile.getRecordPtr(f.readUInt32Fast());
        }
        else if (f == "DATA" && f.size() >= 24)
        {
          offsX = f.readFloat();
          offsY = f.readFloat();
          offsZ = f.readFloat();
          rX = f.readFloat();
          rY = f.readFloat();
          rZ = f.readFloat();
        }
        else if (f == "XSCL" && f.size() >= 4)
        {
          scale = f.readFloat();
        }
        else if (f == "XMSP" && f.size() >= 4)
        {
          refrMSWPFormID = f.readUInt32Fast();
        }
      }
    }
    if (!r2)
      continue;
    if (*r2 == "SCOL" && !enableSCOL)
    {
      addSCOLObjects(*r2, scale, rX, rY, rZ, offsX, offsY, offsZ,
                     refrMSWPFormID);
      continue;
    }
    RenderObject  tmp;
    tmp.tileIndex = -1;
    tmp.z = 0;
    const BaseObject  *o = readModelProperties(tmp, *r2);
    if (!o)
      continue;
    tmp.modelTransform = NIFFile::NIFVertexTransform(scale, rX, rY, rZ,
                                                     offsX, offsY, offsZ);
    if (setScreenAreaUsed(tmp) < 0)
      continue;
    if (debugMode == 1)
      tmp.z = int(r->formID);
    if (tmp.flags & 4)
    {
      getWaterColor(tmp, *r2);
    }
    else if (refrMSWPFormID)
    {
      if (refrMSWPFormID != o->mswpFormID)
      {
        refrMSWPFormID =
            materialSwaps.loadMaterialSwap(ba2File, esmFile, refrMSWPFormID);
      }
      tmp.mswpFormID = refrMSWPFormID;
    }
    objectList.push_back(tmp);
  }
  while ((formID = r->next) != 0U && isRecursive);
}

void Renderer::findObjects(unsigned int formID, int type)
{
  if (!formID)
    return;
  const ESMFile::ESMRecord  *r = esmFile.getRecordPtr(formID);
  if (!r)
    return;
  if (*r == "WRLD")
  {
    r = esmFile.getRecordPtr(0U);
    while (r && r->next)
    {
      r = esmFile.getRecordPtr(r->next);
      if (!r)
        break;
      if (*r == "GRUP" && r->formID == 0 && r->children)
      {
        unsigned int  groupID = r->children;
        while (groupID)
        {
          const ESMFile::ESMRecord  *r2 = esmFile.getRecordPtr(groupID);
          if (!r2)
            break;
          if (*r2 == "GRUP" && r2->formID == 1 && r2->flags == formID &&
              r2->children)
          {
            // world children
            findObjects(r2->children, type, true);
          }
          groupID = r2->next;
        }
      }
    }
  }
  else if (*r == "CELL")
  {
    findObjects(formID, type, false);
    r = esmFile.getRecordPtr(0U);
    while (r)
    {
      if (*r == "GRUP")
      {
        if (r->formID <= 5U && r->children)
        {
          r = esmFile.getRecordPtr(r->children);
          continue;
        }
        else if (r->formID <= 9U && r->formID != 7U && r->flags == formID &&
                 r->children)
        {
          // cell children
          findObjects(r->children, type, true);
        }
      }
      while (r && !r->next && r->parent)
        r = esmFile.getRecordPtr(r->parent);
      if (!(r && r->next))
        break;
      r = esmFile.getRecordPtr(r->next);
    }
  }
  else if (!(*r == "GRUP"))
  {
    findObjects(formID, type, false);
  }
  else if (r->children)
  {
    findObjects(r->children, type, true);
  }
}

void Renderer::sortObjectList()
{
  if (renderPass & 4)
  {
    for (size_t i = 0; i < objectList.size(); )
    {
      if (!(objectList[i].flags & 0x08))
      {
        objectList[i] = objectList[objectList.size() - 1];
        objectList.resize(objectList.size() - 1);
      }
      else
      {
        if (debugMode != 1)
          objectList[i].z = 0x70000000 - objectList[i].z;       // reverse sort
        i++;
      }
    }
  }
  std::map< std::string, unsigned int > modelPathsUsed;
  for (size_t i = 0; i < objectList.size(); i++)
  {
    if (objectList[i].flags & 0x02)
      modelPathsUsed[objectList[i].model.o->modelPath] = 0U;
  }
  unsigned int  n = 0U;
  for (std::map< std::string, unsigned int >::iterator
           i = modelPathsUsed.begin(); i != modelPathsUsed.end(); i++, n++)
  {
    i->second = n;
  }
  for (std::map< unsigned int, BaseObject >::iterator
           i = baseObjects.begin(); i != baseObjects.end(); i++)
  {
    std::map< std::string, unsigned int >::const_iterator j =
        modelPathsUsed.find(i->second.modelPath);
    if (j == modelPathsUsed.end())
      i->second.modelID = 0xFFFFFFFFU;
    else
      i->second.modelID = j->second;
  }
  std::sort(objectList.begin(), objectList.end());
}

void Renderer::clear(unsigned int flags)
{
  if (flags & 0x03)
  {
    size_t  imageDataSize = size_t(width) * size_t(height);
    if (flags & 0x01)
    {
      for (size_t i = 0; i < imageDataSize; i++)
        outBufRGBA[i] = 0U;
    }
    if (flags & 0x02)
    {
      float   tmp = float(zRangeMax);
      for (size_t i = 0; i < imageDataSize; i++)
        outBufZ[i] = tmp;
    }
  }
  if (flags & 0x04)
  {
    if (landData)
    {
      delete landData;
      landData = (LandscapeData *) 0;
    }
    landTextures.clear();
    landTexturesN.clear();
  }
  if (flags & 0x08)
  {
    objectList.clear();
    renderPass = 0;
  }
  if (flags & 0x10)
  {
    for (size_t i = 0; i < renderThreads.size(); i++)
      renderThreads[i].clear();
  }
  if (flags & 0x20)
  {
    for (size_t i = 0; i < nifFiles.size(); i++)
      nifFiles[i].clear();
  }
  if (flags & 0x40)
    textureCache.clear();
}

bool Renderer::isExcludedModel(const std::string& modelPath) const
{
  for (size_t i = 0; i < excludeModelPatterns.size(); i++)
  {
    if (modelPath.find(excludeModelPatterns[i]) != std::string::npos)
      return true;
  }
  return false;
}

bool Renderer::isHighQualityModel(const std::string& modelPath) const
{
  for (size_t i = 0; i < hdModelNamePatterns.size(); i++)
  {
    if (modelPath.find(hdModelNamePatterns[i]) != std::string::npos)
      return true;
  }
  return false;
}

void Renderer::loadModels(unsigned int t, unsigned long long modelIDMask)
{
  for (size_t n = 0; modelIDMask; modelIDMask = modelIDMask >> 1, n++)
  {
    if (!(modelIDMask & 1U))
      continue;
    const BaseObject& o = *(nifFiles[n].o);
    nifFiles[n].clear();
    if (o.modelID == 0xFFFFFFFFU || o.modelPath.empty())
      continue;
    if (renderPass & 4)
    {
      if (std::strncmp(o.modelPath.c_str(), "meshes/sky/", 11) == 0 ||
          std::strncmp(o.modelPath.c_str(), "meshes/effects/ambient/", 23) == 0)
      {
        continue;
      }
    }
    try
    {
      ba2File.extractFile(nifFiles[t].fileBuf, o.modelPath);
      nifFiles[n].nifFile = new NIFFile(&(nifFiles[t].fileBuf.front()),
                                        nifFiles[t].fileBuf.size(), &ba2File);
      bool    isHDModel = bool(o.flags & 0x0040);
      nifFiles[n].nifFile->getMesh(nifFiles[n].meshData, 0U,
                                   (unsigned int) (modelLOD > 0 && !isHDModel),
                                   true);
      size_t  meshCnt = nifFiles[n].meshData.size();
      for (size_t i = 0; i < meshCnt; i++)
      {
        const NIFFile::NIFTriShape& ts = nifFiles[n].meshData[i];
        // check hidden (0x8000) and alpha blending (0x1000) flags
        if (((ts.m.flags >> 10) ^ renderPass) & 0x24U)
          continue;
        nifFiles[n].totalTriangleCnt += size_t(ts.triangleCnt);
      }
    }
    catch (FO76UtilsError&)
    {
      nifFiles[n].clear();
    }
  }
}

void Renderer::loadModelsThread(Renderer *p,
                                unsigned int t, unsigned long long modelIDMask)
{
  try
  {
    p->nifFiles[t].threadErrMsg.clear();
    p->loadModels(t, modelIDMask);
  }
  catch (std::exception& e)
  {
    p->nifFiles[t].threadErrMsg = e.what();
    if (p->nifFiles[t].threadErrMsg.empty())
      p->nifFiles[t].threadErrMsg = "unknown error in load model thread";
  }
}

struct ThreadSortObject
{
  unsigned long long  tileMask;
  size_t  triangleCnt;
  ThreadSortObject()
    : tileMask(0ULL),
      triangleCnt(0)
  {
  }
  inline bool operator<(const ThreadSortObject& r) const
  {
    return (triangleCnt > r.triangleCnt);       // reverse sort
  }
  inline ThreadSortObject& operator+=(const ThreadSortObject& r)
  {
    tileMask |= r.tileMask;
    triangleCnt += r.triangleCnt;
    return (*this);
  }
};

void Renderer::renderObjectList()
{
  clear(0x30);
  std::vector< ThreadSortObject > threadSortBuf(64);
  if (landData)
  {
    for (size_t i = 0; i < renderThreads.size(); i++)
    {
      if (!renderThreads[i].terrainMesh)
        renderThreads[i].terrainMesh = new TerrainMesh();
    }
  }
  unsigned int  modelIDBase = 0xFFFFFFFFU;
  for (size_t i = 0; i < objectList.size(); )
  {
    for (size_t k = 0; k < 64; k++)
    {
      threadSortBuf[k].tileMask = 1ULL << (unsigned int) k;
      threadSortBuf[k].triangleCnt = 0;
    }
    const RenderObject& o = objectList[i];
    unsigned int  modelIDMask = modelBatchCnt - 1U;
    if ((o.flags & 0x02) && (o.model.o->modelID & ~modelIDMask) != modelIDBase)
    {
      // load new set of models
      modelIDBase = o.model.o->modelID & ~modelIDMask;
      for (unsigned int n = 0; n < modelBatchCnt; n++)
        nifFiles[n].clear();
      unsigned long long  m = 0ULL;
      for (size_t j = i; j < objectList.size(); j++)
      {
        const RenderObject& p = objectList[j];
        if (!(p.flags & 0x02))
          continue;
        if ((p.model.o->modelID & ~modelIDMask) != modelIDBase)
          break;
        unsigned int  n = p.model.o->modelID & modelIDMask;
        nifFiles[n].o = p.model.o;
        m = m | (1ULL << n);
      }
      unsigned long long  tmp = 0ULL;
      unsigned int  nThreads = (unsigned int) threadCnt;
      nThreads = (nThreads < modelBatchCnt ? nThreads : modelBatchCnt);
      for (unsigned int k = 0; k < modelBatchCnt; k = k + nThreads)
        tmp = tmp | (1ULL << k);
      for (unsigned int k = 0; k < nThreads; k++, tmp = tmp << 1)
      {
        nifFiles[k].loadThread =
            new std::thread(loadModelsThread, this, k, m & tmp);
      }
      for (unsigned int k = 0; k < nThreads; k++)
      {
        nifFiles[k].loadThread->join();
        delete nifFiles[k].loadThread;
        nifFiles[k].loadThread = (std::thread *) 0;
      }
      for (unsigned int k = 0; k < nThreads; k++)
      {
        if (!nifFiles[k].threadErrMsg.empty())
          throw FO76UtilsError(1, nifFiles[k].threadErrMsg.c_str());
      }
      for (unsigned int n = 0; n < modelBatchCnt; n++)
      {
        if (!nifFiles[n].totalTriangleCnt)
          nifFiles[n].totalTriangleCnt = 1;
      }
    }
    size_t  j = i;
    while (j < objectList.size())
    {
      const RenderObject& p = objectList[j];
      if ((o.tileIndex ^ p.tileIndex) & ~63)
        break;
      if ((p.flags & 2) && (p.model.o->modelID & ~modelIDMask) != modelIDBase)
        break;
      size_t  triangleCnt = 2;
      if (p.flags & 0x02)
      {
        triangleCnt =
            nifFiles[p.model.o->modelID & modelIDMask].totalTriangleCnt;
      }
      else if ((p.flags & 0x01) && landData)
      {
        triangleCnt = size_t(landData->getCellResolution());
        if (triangleCnt > 24)
          triangleCnt = triangleCnt - 8;
        triangleCnt = triangleCnt * triangleCnt * 2U;
      }
      threadSortBuf[p.tileIndex & 63].triangleCnt += triangleCnt;
      j++;
    }
    // sort in descending order by triangle count
    std::sort(threadSortBuf.begin(), threadSortBuf.end());
    for (size_t k = size_t(threadCnt); k < threadSortBuf.size(); k++)
    {
      if (threadSortBuf[k].triangleCnt < 1)
        break;
      // find the thread with the lowest triangle count
      size_t  n = 0;
      for (size_t l = 1; l < size_t(threadCnt); l++)
      {
        if (threadSortBuf[l].triangleCnt < threadSortBuf[n].triangleCnt)
          n = l;
      }
      // add the remaining area with the highest triangle count
      threadSortBuf[n] += threadSortBuf[k];
    }
    size_t  threadsUsed = size_t(threadCnt);
    while (threadsUsed > 1 && threadSortBuf[threadsUsed - 1].triangleCnt < 1)
      threadsUsed--;
    for (size_t k = 1; k < threadsUsed; k++)
    {
      renderThreads[k].t = new std::thread(threadFunction, this, k, i, j,
                                           threadSortBuf[k].tileMask);
    }
    if (threadSortBuf[0].triangleCnt > 0)
      renderThread(0, i, j, threadSortBuf[0].tileMask);
    for (size_t k = 1; k < threadsUsed; k++)
      renderThreads[k].join();
    for (size_t k = 1; k < threadsUsed; k++)
    {
      if (!renderThreads[k].errMsg.empty())
        throw FO76UtilsError(1, renderThreads[k].errMsg.c_str());
    }
    // render any remaining objects that were skipped due to incorrect bounds
    for (size_t k = 0; k < threadsUsed; k++)
    {
      for (size_t l = 0; l < renderThreads[k].objectsRemaining.size(); l++)
        renderObject(renderThreads[k], renderThreads[k].objectsRemaining[l]);
      renderThreads[k].objectsRemaining.clear();
    }
    i = j;
    textureCache.shrinkTextureCache();
    if (verboseMode)
    {
      std::fprintf(stderr, "\r    %7u / %7u  ",
                   (unsigned int) i, (unsigned int) objectList.size());
    }
  }
  clear(0x20);
  if (verboseMode)
    std::fputc('\n', stderr);
}

bool Renderer::renderObject(RenderThread& t, size_t i,
                            unsigned long long tileMask)
{
  const RenderObject& p = objectList[i];
  t.renderer->setDebugMode(debugMode, (unsigned int) p.z);
  if (p.flags & 0x02)                   // object or water mesh
  {
    NIFFile::NIFVertexTransform vt(p.modelTransform);
    vt *= viewTransform;
    size_t  n = p.model.o->modelID & (modelBatchCnt - 1U);
    t.sortBuf.clear();
    t.sortBuf.reserve(nifFiles[n].meshData.size());
    for (size_t j = 0; j < nifFiles[n].meshData.size(); j++)
    {
      const NIFFile::NIFTriShape& ts = nifFiles[n].meshData[j];
      // check hidden (0x8000) and alpha blending (0x1000) flags
      if (((ts.m.flags >> 10) ^ renderPass) & 0x24U)
      {
        if ((ts.m.flags & BGSMFile::Flag_TSAlphaBlending) &&
            (ts.m.texturePathMask | (ts.m.flags & BGSMFile::Flag_TSWater)))
        {
          objectList[i].flags |= (unsigned short) 0x08; // uses alpha blending
        }
        continue;
      }
      if (!ts.triangleCnt)
        continue;
      NIFFile::NIFBounds  b;
      ts.calculateBounds(b, &vt);
      int     x0 = roundFloat(b.xMin());
      int     y0 = roundFloat(b.yMin());
      int     x1 = roundFloat(b.xMax());
      int     y1 = roundFloat(b.yMax());
      unsigned long long  m = calculateTileMask(x0, y0, x1, y1);
      if (!m)
        continue;
      if (m & ~tileMask)
        return false;
      x0 = (x0 > 0 ? x0 : 0);
      y0 = (y0 > 0 ? y0 : 0);
      x1 = (x1 < (width - 1) ? x1 : (width - 1)) + 1;
      y1 = (y1 < (height - 1) ? y1 : (height - 1)) + 1;
      bool    isVisible = false;
      for (int y = y0; y < y1; y++)
      {
        FloatVector4  zMax(0.0f);
        const float *zPtr = outBufZ + (size_t(y) * size_t(width) + size_t(x0));
        int     w = x1 - x0;
        for ( ; w >= 4; w = w - 4, zPtr = zPtr + 4)
          zMax.maxValues(FloatVector4(zPtr));
        zMax.maxValues(FloatVector4(zMax[1], zMax[0], zMax[3], zMax[2]));
        float   tmp = (zMax[0] > zMax[2] ? zMax[0] : zMax[2]);
        for ( ; w > 0; w--, zPtr++)
          tmp = (*zPtr > tmp ? *zPtr : tmp);
        if (tmp < b.zMin())
          continue;
        isVisible = true;
        break;
      }
      if (!isVisible)
        continue;
      t.sortBuf.push_back(TriShapeSortObject(ts, b.zMin()));
      if (BRANCH_UNLIKELY(ts.m.flags & BGSMFile::Flag_TSOrdered))
        TriShapeSortObject::orderedNodeFix(t.sortBuf, nifFiles[n].meshData);
    }
    if (t.sortBuf.size() < 1)
      return true;
    std::stable_sort(t.sortBuf.begin(), t.sortBuf.end());
    bool    isHDModel = bool(p.flags & 0x40);
    for (size_t j = 0; j < t.sortBuf.size(); j++)
    {
      *(t.renderer) = *(t.sortBuf[j].ts);
      const DDSTexture  *textures[10];
      unsigned int  textureMask = 0U;
      if (BRANCH_UNLIKELY(t.renderer->m.flags & BGSMFile::Flag_TSWater))
      {
        t.renderer->setRenderMode(3U | renderMode);
        t.renderer->m.setWaterColor(p.mswpFormID, waterReflectionLevel);
        textures[1] = textureCache.loadTexture(ba2File, defaultWaterTexture,
                                               t.fileBuf, 0);
        if (textures[1])
          textureMask |= 0x0002U;
        textures[4] = textureCache.loadTexture(ba2File, defaultEnvMap,
                                               t.fileBuf, 0);
        if (textures[4])
          textureMask |= 0x0010U;
      }
      else
      {
        if (p.model.o->mswpFormID && p.mswpFormID != p.model.o->mswpFormID)
          materialSwaps.materialSwap(*(t.renderer), p.model.o->mswpFormID);
        if (p.mswpFormID)
          materialSwaps.materialSwap(*(t.renderer), p.mswpFormID);
        if (p.flags & 0x80)
        {
          t.renderer->m.s.gradientMapV =
              float(int(p.flags >> 8)) * (1.0f / 255.0f);
        }
        unsigned int  texturePathMask = t.renderer->m.texturePathMask;
        texturePathMask &= ((((unsigned int) t.renderer->m.flags & 0x80U) >> 5)
                            | (!isHDModel ? 0x0009U : 0x037BU));
        t.renderer->setRenderMode((!isHDModel ? 0U : 3U) | renderMode);
        if (BRANCH_UNLIKELY(!enableTextures))
        {
          if (t.renderer->m.alphaThresholdFloat > 0.0f ||
              (texturePathMask & 0x0008U))
          {
            texturePathMask &= ~0x0008U;
            textures[3] = &whiteTexture;
            textureMask |= 0x0008U;
          }
          else
          {
            texturePathMask &= ~0x0001U;
            textures[0] = &whiteTexture;
            textureMask |= 0x0001U;
          }
        }
        else if (!(texturePathMask & 0x0001U) ||
                 t.renderer->m.texturePaths[0].find("/temp_ground")
                 != std::string::npos)
        {
          continue;
        }
        for (unsigned int m = 0x00080200U; texturePathMask; m = m >> 1)
        {
          unsigned int  tmp = texturePathMask & m;
          if (!tmp)
            continue;
          int     k = FloatVector4::log2Int(int(tmp));
          bool    waitFlag = false;
          textures[k] = textureCache.loadTexture(
                            ba2File, t.renderer->m.texturePaths[k], t.fileBuf,
                            (!(m & 0x0018U) ? textureMip : 0),
                            (m > 0x03FFU ? &waitFlag : (bool *) 0));
          if (!waitFlag)
          {
            texturePathMask &= ~tmp;
            if (textures[k])
              textureMask |= tmp;
          }
        }
        if (!(textureMask & 0x0010U) && t.renderer->m.s.envMapScale > 0.0f &&
            isHDModel)
        {
          textures[4] = textureCache.loadTexture(ba2File, defaultEnvMap,
                                                 t.fileBuf, 0);
          if (textures[4])
            textureMask |= 0x0010U;
        }
      }
      t.renderer->drawTriShape(
          p.modelTransform, viewTransform, lightX, lightY, lightZ,
          textures, textureMask);
    }
  }
  else if (p.flags & 0x01)              // terrain
  {
    if (landData && t.terrainMesh)
    {
      int     landTxtScale = 0;
      for (int j = landData->getCellResolution(); j < cellTextureResolution; )
      {
        landTxtScale++;
        j = j << 1;
      }
      const DDSTexture * const  *ltxtN = (DDSTexture **) 0;
      if (landTexturesN.size() >= landTextures.size())
        ltxtN = &(landTexturesN.front());
      t.terrainMesh->createMesh(
          *landData, landTxtScale,
          p.model.t.x0, p.model.t.y0, p.model.t.x1, p.model.t.y1,
          &(landTextures.front()), ltxtN, landTextures.size(),
          landTextureMip - float(int(landTextureMip)),
          landTxtRGBScale, landTxtDefColor);
      t.renderer->setRenderMode((hdModelNamePatterns.size() > 0 ? 1U : 0U)
                                | renderMode);
      *(t.renderer) = *(t.terrainMesh);
      t.renderer->drawTriShape(
          p.modelTransform, viewTransform, lightX, lightY, lightZ,
          t.terrainMesh->getTextures(), t.terrainMesh->getTextureMask());
    }
  }
  else if ((p.flags & renderPass) & 0x04)       // water cell
  {
    NIFFile::NIFTriShape  tmp;
    NIFFile::NIFVertex    vTmp[4];
    NIFFile::NIFTriangle  tTmp[2];
    int     x0 = p.model.t.x0;
    int     y0 = p.model.t.y0;
    int     x1 = p.model.t.x1;
    int     y1 = p.model.t.y1;
    for (int j = 0; j < 4; j++)
    {
      vTmp[j].x = float(j == 0 || j == 3 ? x0 : x1);
      vTmp[j].y = float(j == 0 || j == 1 ? y0 : y1);
      vTmp[j].u = (unsigned short) (j == 0 || j == 3 ? 0x0000 : 0x4000);
      vTmp[j].v = (unsigned short) (j == 0 || j == 1 ? 0x0000 : 0x4000);
    }
    tTmp[0].v0 = 0;
    tTmp[0].v1 = 1;
    tTmp[0].v2 = 2;
    tTmp[1].v0 = 0;
    tTmp[1].v1 = 2;
    tTmp[1].v2 = 3;
    tmp.vertexCnt = 4;
    tmp.triangleCnt = 2;
    tmp.vertexData = vTmp;
    tmp.triangleData = tTmp;
    tmp.m.nifVersion = (esmFile.getESMVersion() < 0xC0U ? 130U : 155U);
    tmp.m.setWaterColor(p.mswpFormID, waterReflectionLevel);
    t.renderer->setRenderMode(3U | renderMode);
    *(t.renderer) = tmp;
    const DDSTexture  *textures[5];
    unsigned int  textureMask = 0U;
    textures[1] = textureCache.loadTexture(ba2File, defaultWaterTexture,
                                           t.fileBuf, 0);
    if (textures[1])
      textureMask |= 0x0002U;
    textures[4] = textureCache.loadTexture(ba2File, defaultEnvMap,
                                           t.fileBuf, 0);
    if (textures[4])
      textureMask |= 0x0010U;
    t.renderer->drawTriShape(
        p.modelTransform, viewTransform, lightX, lightY, lightZ,
        textures, textureMask);
  }
  return true;
}

void Renderer::renderThread(size_t threadNum, size_t startPos, size_t endPos,
                            unsigned long long tileIndexMask)
{
  RenderThread& t = renderThreads[threadNum];
  if (!t.renderer)
    return;
  unsigned long long  tileMask = 0ULL;
  for (size_t i = startPos; i < endPos; i++)
  {
    int     tileIndex = objectList[i].tileIndex;
    if (tileIndex < 0 ||
        !((1ULL << (unsigned int) (tileIndex & 63)) & tileIndexMask) ||
        ((1ULL << (unsigned int) (tileIndex & 63)) & ~tileIndexMask))
    {
      continue;
    }
    if (BRANCH_UNLIKELY(!tileMask))
    {
      // calculate mask of areas this thread is allowed to access
      if (tileIndex < 64 || tileIndexMask == ~0ULL)
      {
        tileMask = tileIndexMask;
      }
      else if (tileIndex < 1344)
      {
        for (int l = 0; l < 64; l++)
        {
          if (!(tileIndexMask & (1ULL << (unsigned int) l)))
            continue;
          int     n = (tileIndex & ~63) + l - (tileIndex < 320 ? 64 : 320);
          int     x0 = (n & 6) + ((n >> 6) & (tileIndex < 320 ? 1 : 3));
          int     y0 = ((n >> 3) & 6) + ((n >> (tileIndex < 320 ? 7 : 8)) & 3);
          n = (tileIndex < 320 ? 2 : 4);
          for (int j = 0; j < n && (y0 + j) < 8; j++)
          {
            for (int k = 0; k < n && (x0 + k) < 8; k++)
              tileMask = tileMask | (1ULL << (unsigned int) ((j << 3) | k));
          }
        }
      }
      else
      {
        tileMask = ~0ULL;
      }
    }
    if (!renderObject(t, i, tileMask))
      t.objectsRemaining.push_back((unsigned int) i);
  }
}

void Renderer::threadFunction(Renderer *p, size_t threadNum,
                              size_t startPos, size_t endPos,
                              unsigned long long tileIndexMask)
{
  try
  {
    p->renderThreads[threadNum].errMsg.clear();
    p->renderThread(threadNum, startPos, endPos, tileIndexMask);
  }
  catch (std::exception& e)
  {
    p->renderThreads[threadNum].errMsg = e.what();
    if (p->renderThreads[threadNum].errMsg.empty())
      p->renderThreads[threadNum].errMsg = "unknown error in render thread";
  }
}

Renderer::Renderer(int imageWidth, int imageHeight,
                   const BA2File& archiveFiles, ESMFile& masterFiles,
                   std::uint32_t *bufRGBA, float *bufZ, int zMax)
  : outBufRGBA(bufRGBA),
    outBufZ(bufZ),
    width(imageWidth),
    height(imageHeight),
    ba2File(archiveFiles),
    esmFile(masterFiles),
    viewTransform(0.0625f, 3.14159265f, 0.0f, 0.0f,
                  float(imageWidth) * 0.5f, float(imageHeight) * 0.5f,
                  32768.0f),
    lightX(-0.6667f),
    lightY(0.6667f),
    lightZ(0.3333f),
    textureMip(2),
    landTextureMip(3.0f),
    landTxtRGBScale(1.0f),
    landTxtDefColor(0x003F3F3FU),
    landData((LandscapeData *) 0),
    cellTextureResolution(256),
    defaultWaterLevel(0.0f),
    modelLOD(0),
    distantObjectsOnly(false),
    noDisabledObjects(false),
    enableSCOL(false),
    enableAllObjects(false),
    enableTextures(true),
    renderMode((unsigned char) ((masterFiles.getESMVersion() >> 4) & 0x0CU)),
    debugMode(0),
    renderPass(0),
    threadCnt(0),
    waterColor(0xFFFFFFFFU),
    waterReflectionLevel(1.0f),
    zRangeMax(zMax),
    verboseMode(true),
    useESMWaterColors(true),
    bufAllocFlags((unsigned char) (int(!bufRGBA) | (int(!bufZ) << 1))),
    whiteTexture(0xFFFFFFFFU)
{
  if (!renderMode)
    renderMode = 4;
  size_t  imageDataSize = size_t(width) * size_t(height);
  if (bufAllocFlags & 0x01)
    outBufRGBA = new std::uint32_t[imageDataSize];
  if (bufAllocFlags & 0x02)
    outBufZ = new float[imageDataSize];
  clear(bufAllocFlags);
  renderThreads.reserve(16);
  nifFiles.resize(modelBatchCnt);
  setThreadCount(-1);
}

Renderer::~Renderer()
{
  clear(0x7C);
  deallocateBuffers(0x03);
}

unsigned int Renderer::findParentWorld(ESMFile& esmFile, unsigned int formID)
{
  if (!formID)
    return 0xFFFFFFFFU;
  const ESMFile::ESMRecord  *r = esmFile.getRecordPtr(formID);
  if (!(r && (*r == "WRLD" || *r == "GRUP" || *r == "CELL" || *r == "REFR")))
    return 0xFFFFFFFFU;
  if (!(*r == "WRLD"))
  {
    while (!(*r == "GRUP" && r->formID == 1U))
    {
      if (!r->parent)
        return 0U;
      r = esmFile.getRecordPtr(r->parent);
      if (!r)
        return 0U;
    }
    formID = r->flags;
    r = esmFile.getRecordPtr(formID);
  }
  return formID;
}

void Renderer::clear()
{
  clear(0x7C);
  baseObjects.clear();
}

void Renderer::clearImage()
{
  clear(0x03);
}

void Renderer::deallocateBuffers(unsigned int mask)
{
  if (((bufAllocFlags & mask) & 0x02) && outBufZ)
  {
    delete[] outBufZ;
    outBufZ = (float *) 0;
  }
  if (((bufAllocFlags & mask) & 0x01) && outBufRGBA)
  {
    delete[] outBufRGBA;
    outBufRGBA = (std::uint32_t *) 0;
  }
  bufAllocFlags = bufAllocFlags & ~mask;
}

void Renderer::setViewTransform(
    float scale, float rotationX, float rotationY, float rotationZ,
    float offsetX, float offsetY, float offsetZ)
{
  NIFFile::NIFVertexTransform t(scale, rotationX, rotationY, rotationZ,
                                offsetX, offsetY, offsetZ);
  viewTransform = t;
}

void Renderer::setLightDirection(float rotationY, float rotationZ)
{
  NIFFile::NIFVertexTransform t(1.0f, 0.0f, rotationY, rotationZ,
                                0.0f, 0.0f, 0.0f);
  lightX = t.rotateZX;
  lightY = t.rotateZY;
  lightZ = t.rotateZZ;
}

void Renderer::setThreadCount(int n)
{
  if (n <= 0)
    n = int(std::thread::hardware_concurrency());
  int     maxThreads = int(renderThreads.capacity());
  n = (n > 1 ? (n < maxThreads ? n : maxThreads) : 1);
  if (n == threadCnt)
    return;
  threadCnt = n;
  renderThreads.resize(size_t(n));
  for (size_t i = 0; i < renderThreads.size(); i++)
  {
    if (!renderThreads[i].renderer)
    {
      renderThreads[i].renderer =
          new Plot3D_TriShape(outBufRGBA, outBufZ, width, height, renderMode);
    }
  }
}

void Renderer::addExcludeModelPattern(const std::string& s)
{
  if (s.empty())
    return;
  for (size_t i = 0; i < excludeModelPatterns.size(); i++)
  {
    if (s.find(excludeModelPatterns[i]) != std::string::npos)
      return;
    if (excludeModelPatterns[i].find(s) != std::string::npos)
      excludeModelPatterns.erase(excludeModelPatterns.begin() + i);
  }
  excludeModelPatterns.push_back(s);
}

void Renderer::addHDModelPattern(const std::string& s)
{
  if (s.empty())
    return;
  for (size_t i = 0; i < hdModelNamePatterns.size(); i++)
  {
    if (s.find(hdModelNamePatterns[i]) != std::string::npos)
      return;
    if (hdModelNamePatterns[i].find(s) != std::string::npos)
      hdModelNamePatterns.erase(hdModelNamePatterns.begin() + i);
  }
  hdModelNamePatterns.push_back(s);
}

void Renderer::setDefaultEnvMap(const std::string& s)
{
  defaultEnvMap = s;
}

void Renderer::setWaterTexture(const std::string& s)
{
  defaultWaterTexture = s;
}

void Renderer::setRenderParameters(
    int lightColor, int ambientColor, int envColor,
    float lightLevel, float envLevel, float rgbScale,
    float reflZScale, int waterUVScale)
{
  if (renderThreads.size() < 1)
    return;
  FloatVector4  c(bgraToRGBA((std::uint32_t) lightColor));
  FloatVector4  a(bgraToRGBA((std::uint32_t) ambientColor & 0x00FFFFFFU));
  FloatVector4  e(bgraToRGBA((std::uint32_t) envColor));
  FloatVector4  l(lightLevel, envLevel, rgbScale, 0.0f);
  l = ((l * -0.01728125f + 0.17087940f) * l + 0.83856107f) * l + 0.00784078f;
  l *= l;
  c = c.srgbExpand() * l[0];
  e = e.srgbExpand() * l[1];
  if (ambientColor >= 0)
  {
    a.srgbExpand();
  }
  else
  {
    a = renderThreads[0].renderer->cubeMapToAmbient(
            textureCache.loadTexture(
                ba2File, defaultEnvMap, renderThreads[0].fileBuf, 0));
  }
  for (size_t i = 0; i < renderThreads.size(); i++)
  {
    renderThreads[i].renderer->setLighting(c, a, e, l[2]);
    renderThreads[i].renderer->setEnvMapOffset(
        float(width) * -0.5f, float(height) * -0.5f,
        float(height) * reflZScale);
    renderThreads[i].renderer->setWaterUVScale(1.0f / float(waterUVScale));
  }
}

static int calculateLandTxtMip(long fileSize)
{
  int     m = 0;
  while ((8L << (m + m)) <= (fileSize - 128L))
    m++;
  // return log2(resolution)
  return (m - 1);
}

void Renderer::loadTerrain(const char *btdFileName,
                           unsigned int worldID, unsigned int defTxtID,
                           int mipLevel, int xMin, int yMin, int xMax, int yMax)
{
  clear(0x44);
  if (verboseMode)
    std::fprintf(stderr, "Loading terrain data\n");
  landData = new LandscapeData(&esmFile, btdFileName, &ba2File, 0x0B, worldID,
                               defTxtID, mipLevel, xMin, yMin, xMax, yMax);
  defaultWaterLevel = landData->getWaterLevel();
  if (useESMWaterColors)
  {
    const ESMFile::ESMRecord  *r =
        esmFile.getRecordPtr(landData->getWaterFormID());
    if (r && *r == "WATR")
      waterColor = Renderer_Base::getWaterColor(esmFile, *r, waterColor);
  }
  if (verboseMode)
    std::fprintf(stderr, "Loading landscape textures\n");
  landTextures.resize(landData->getTextureCount(), (DDSTexture *) 0);
  if (cellTextureResolution > landData->getCellResolution())
    landTexturesN.resize(landData->getTextureCount(), (DDSTexture *) 0);
  std::vector< unsigned char >& fileBuf(renderThreads[0].fileBuf);
  int     mipLevelD = textureMip + int(landTextureMip);
  for (size_t i = 0; i < landTextures.size(); i++)
  {
    if (!enableTextures)
    {
      landTextures[i] = &whiteTexture;
    }
    else
    {
      landTextures[i] = textureCache.loadTexture(
                            ba2File, landData->getTextureDiffuse(i), fileBuf,
                            mipLevelD);
    }
    if (i < landTexturesN.size())
    {
      long    fileSizeD = ba2File.getFileSize(landData->getTextureDiffuse(i));
      long    fileSizeN = ba2File.getFileSize(landData->getTextureNormal(i));
      int     mipLevelN = mipLevelD + calculateLandTxtMip(fileSizeN)
                          - calculateLandTxtMip(fileSizeD);
      mipLevelN = (mipLevelN > 0 ? (mipLevelN < 15 ? mipLevelN : 15) : 0);
      landTexturesN[i] = textureCache.loadTexture(
                             ba2File, landData->getTextureNormal(i), fileBuf,
                             mipLevelN);
    }
  }
}

void Renderer::renderTerrain(unsigned int worldID)
{
  if (verboseMode)
    std::fprintf(stderr, "Rendering terrain\n");
  if (!worldID)
    worldID = getDefaultWorldID();
  clear(0x38);
  renderPass = 1;
  findObjects(worldID, 0);
  sortObjectList();
  renderObjectList();
}

void Renderer::renderObjects(unsigned int formID)
{
  if (verboseMode)
    std::fprintf(stderr, "Rendering objects\n");
  if (!formID)
    formID = getDefaultWorldID();
  if (useESMWaterColors && waterColor == 0xFFFFFFFFU)
    waterColor = 0xC0302010U;           // default water color
  clear(0x38);
  renderPass = 2;
  baseObjects.clear();
  findObjects(formID, 1);
  sortObjectList();
  renderObjectList();
  if (verboseMode)
    std::fprintf(stderr, "Rendering water and transparent objects\n");
  renderPass = 4;
  sortObjectList();
  renderObjectList();
}

static const char *usageStrings[] =
{
  "Usage: render INFILE.ESM[,...] OUTFILE.DDS W H ARCHIVEPATH [OPTIONS...]",
  "",
  "Options:",
  "    --help              print usage",
  "    --list-defaults     print defaults for all options, and exit",
  "    --                  remaining options are file names",
  "    -threads INT        set the number of threads to use",
  "    -debug INT          set debug render mode (0: disabled, 1: form IDs,",
  "                        2: depth, 3: normals, 4: diffuse, 5: light only)",
  "    -scol BOOL          enable the use of pre-combined meshes",
  "    -a                  render all object types",
  "    -textures BOOL      make all diffuse textures white if false",
  "    -txtcache INT       texture cache size in megabytes",
  "    -ssaa BOOL          render at double resolution and downsample",
  "    -f INT              output format, 0: RGB24, 1: A8R8G8B8, 2: RGB10A2",
  "    -q                  do not print messages other than errors",
  "",
  "    -btd FILENAME.BTD   read terrain data from Fallout 76 .btd file",
  "    -w FORMID           form ID of world, cell, or object to render",
  "    -r X0 Y0 X1 Y1      terrain cell range, X0,Y0 = SW to X1,Y1 = NE",
  "    -l INT              level of detail to use from BTD file (0 to 4)",
  "    -deftxt FORMID      form ID of default land texture",
  "    -defclr 0x00RRGGBB  default color for untextured terrain",
  "    -ltxtres INT        land texture resolution per cell",
  "    -mip INT            base mip level for all textures",
  "    -lmip FLOAT         additional mip level for land textures",
  "    -lmult FLOAT        land texture RGB level scale",
  "",
  "    -view SCALE RX RY RZ OFFS_X OFFS_Y OFFS_Z",
  "                        set transform from world coordinates to image",
  "                        coordinates, rotations are in degrees",
  "    -cam SCALE RX RY RZ X Y Z",
  "                        set view transform for camera position X,Y,Z",
  "    -zrange MIN MAX     limit view Z range to MIN <= Z < MAX",
  "    -light SCALE RY RZ  set RGB scale and Y, Z rotation (0, 0 = top)",
  "    -lcolor LMULT LCOLOR EMULT ECOLOR ACOLOR",
  "                        set light source, environment, and ambient light",
  "                        colors and levels (colors in 0xRRGGBB format)",
  "    -rscale FLOAT       reflection view vector Z scale",
  "",
  "    -mlod INT           set level of detail for models, 0 (best) to 4",
  "    -vis BOOL           render only objects visible from distance",
  "    -ndis BOOL          do not render initially disabled objects",
  "    -hqm STRING         add high quality model path name pattern",
  "    -xm STRING          add excluded model path name pattern",
  "",
  "    -env FILENAME.DDS   default environment map texture path in archives",
  "    -wtxt FILENAME.DDS  water normal map texture path in archives",
  "    -watercolor UINT32  water color (A7R8G8B8), 0 disables water",
  "    -wrefl FLOAT        water environment map scale",
  "    -wscale INT         water texture tile size",
  (char *) 0
};

int main(int argc, char **argv)
{
  int     err = 1;
  try
  {
    std::vector< const char * > args;
    int     threadCnt = -1;
    int     textureCacheSize = 1024;
    bool    verboseMode = true;
    bool    distantObjectsOnly = false;
    bool    noDisabledObjects = true;
    bool    enableDownscale = false;
    bool    enableSCOL = false;
    bool    enableAllObjects = false;
    bool    enableTextures = true;
    unsigned char debugMode = 0;
    unsigned int  formID = 0U;
    int     btdLOD = 0;
    const char  *btdPath = (char *) 0;
    int     terrainX0 = -32768;
    int     terrainY0 = -32768;
    int     terrainX1 = 32767;
    int     terrainY1 = 32767;
    unsigned int  defTxtID = 0U;
    std::uint32_t ltxtDefColor = 0x003F3F3FU;
    int     ltxtResolution = 128;
    int     textureMip = 2;
    float   landTextureMip = 3.0f;
    float   landTextureMult = 1.0f;
    int     modelLOD = 0;
    std::uint32_t waterColor = 0x7FFFFFFFU;
    float   waterReflectionLevel = 1.0f;
    int     zMin = 0;
    int     zMax = 16777216;
    float   viewScale = 0.0625f;
    float   viewRotationX = 180.0f;
    float   viewRotationY = 0.0f;
    float   viewRotationZ = 0.0f;
    float   viewOffsX = 0.0f;
    float   viewOffsY = 0.0f;
    float   viewOffsZ = 32768.0f;
    float   rgbScale = 1.0f;
    float   lightRotationY = 70.5288f;
    float   lightRotationZ = 135.0f;
    int     lightColor = 0x00FFFFFF;
    int     ambientColor = -1;
    int     envColor = 0x00FFFFFF;
    float   lightLevel = 1.0f;
    float   envLevel = 1.0f;
    float   reflZScale = 2.0f;
    int     waterUVScale = 2048;
    int     outputFormat = 0;
    const char  *defaultEnvMap =
        "textures/shared/cubemaps/mipblur_defaultoutside1.dds";
    const char  *waterTexture = "textures/water/defaultwater.dds";
    std::vector< const char * > hdModelNamePatterns;
    std::vector< const char * > excludeModelPatterns;
    float   d = float(std::atan(1.0) / 45.0);   // degrees to radians

    for (int i = 1; i < argc; i++)
    {
      if (std::strcmp(argv[i], "--") == 0)
      {
        while (++i < argc)
          args.push_back(argv[i]);
        break;
      }
      else if (std::strcmp(argv[i], "--help") == 0)
      {
        args.clear();
        err = 0;
        break;
      }
      else if (std::strcmp(argv[i], "--list-defaults") == 0)
      {
        std::printf("-threads %d", threadCnt);
        if (threadCnt <= 0)
        {
          std::printf(" (defaults to hardware threads: %d)",
                      int(std::thread::hardware_concurrency()));
        }
        std::printf("\n-debug %d\n", int(debugMode));
        std::printf("-scol %d\n", int(enableSCOL));
        std::printf("-textures %d\n", int(enableTextures));
        std::printf("-txtcache %d\n", textureCacheSize);
        std::printf("-ssaa %d\n", int(enableDownscale));
        std::printf("-f %d\n", outputFormat);
        std::printf("-w 0x%08X", formID);
        if (!formID)
          std::printf(" (defaults to 0x0000003C or 0x0025DA15)");
        std::printf("\n-r %d %d %d %d\n",
                    terrainX0, terrainY0, terrainX1, terrainY1);
        std::printf("-l %d\n", btdLOD);
        std::printf("-deftxt 0x%08X\n", defTxtID);
        std::printf("-defclr 0x%08X\n", (unsigned int) ltxtDefColor);
        std::printf("-ltxtres %d\n", ltxtResolution);
        std::printf("-mip %d\n", textureMip);
        std::printf("-lmip %.1f\n", landTextureMip);
        std::printf("-lmult %.1f\n", landTextureMult);
        std::printf("-view %.6f %.1f %.1f %.1f %.1f %.1f %.1f\n",
                    viewScale, viewRotationX, viewRotationY, viewRotationZ,
                    viewOffsX, viewOffsY, viewOffsZ);
        {
          NIFFile::NIFVertexTransform
              vt(1.0f, viewRotationX * d, viewRotationY * d, viewRotationZ * d,
                 0.0f, 0.0f, 0.0f);
          std::printf("    Rotation matrix:\n");
          std::printf("        X: %9.6f %9.6f %9.6f\n",
                      vt.rotateXX, vt.rotateXY, vt.rotateXZ);
          std::printf("        Y: %9.6f %9.6f %9.6f\n",
                      vt.rotateYX, vt.rotateYY, vt.rotateYZ);
          std::printf("        Z: %9.6f %9.6f %9.6f\n",
                      vt.rotateZX, vt.rotateZY, vt.rotateZZ);
        }
        std::printf("-zrange %d %d\n", zMin, zMax);
        std::printf("-light %.1f %.4f %.4f\n",
                    rgbScale, lightRotationY, lightRotationZ);
        {
          NIFFile::NIFVertexTransform
              vt(1.0f, 0.0f, lightRotationY * d, lightRotationZ * d,
                 0.0f, 0.0f, 0.0f);
          std::printf("    Light vector: %9.6f %9.6f %9.6f\n",
                      vt.rotateZX, vt.rotateZY, vt.rotateZZ);
        }
        std::printf("-lcolor %.3f 0x%06X %.3f 0x%06X ",
                    lightLevel, (unsigned int) lightColor,
                    envLevel, (unsigned int) envColor);
        if (ambientColor < 0)
          std::printf("%d (calculated from default cube map)\n", ambientColor);
        else
          std::printf("0x%06X\n", (unsigned int) ambientColor);
        std::printf("-rscale %.3f\n", reflZScale);
        std::printf("-mlod %d\n", modelLOD);
        std::printf("-vis %d\n", int(distantObjectsOnly));
        std::printf("-ndis %d\n", int(noDisabledObjects));
        std::printf("-watercolor 0x%08X\n", (unsigned int) waterColor);
        std::printf("-wrefl %.3f\n", waterReflectionLevel);
        std::printf("-wscale %d\n", waterUVScale);
        return 0;
      }
      else if (argv[i][0] != '-')
      {
        args.push_back(argv[i]);
      }
      else if (std::strcmp(argv[i], "-threads") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        threadCnt = int(parseInteger(argv[i], 10, "invalid number of threads",
                                     1, 16));
      }
      else if (std::strcmp(argv[i], "-debug") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        debugMode = (unsigned char) parseInteger(argv[i], 10,
                                                 "invalid debug mode", 0, 5);
      }
      else if (std::strcmp(argv[i], "-scol") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        enableSCOL =
            bool(parseInteger(argv[i], 0, "invalid argument for -scol", 0, 1));
      }
      else if (std::strcmp(argv[i], "-a") == 0)
      {
        enableAllObjects = true;
      }
      else if (std::strcmp(argv[i], "-textures") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        enableTextures =
            bool(parseInteger(argv[i], 0, "invalid argument for -textures",
                              0, 1));
      }
      else if (std::strcmp(argv[i], "-txtcache") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        textureCacheSize =
            int(parseInteger(argv[i], 0, "invalid texture cache size",
                             256, 4095));
      }
      else if (std::strcmp(argv[i], "-ssaa") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        enableDownscale =
            bool(parseInteger(argv[i], 0, "invalid argument for -ssaa", 0, 1));
      }
      else if (std::strcmp(argv[i], "-f") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        outputFormat =
            int(parseInteger(argv[i], 10, "invalid output format", 0, 2));
      }
      else if (std::strcmp(argv[i], "-q") == 0)
      {
        verboseMode = false;
      }
      else if (std::strcmp(argv[i], "-btd") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        btdPath = argv[i];
      }
      else if (std::strcmp(argv[i], "-w") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        formID = (unsigned int) parseInteger(argv[i], 0, "invalid form ID",
                                             0, 0x0FFFFFFF);
      }
      else if (std::strcmp(argv[i], "-r") == 0)
      {
        if ((i + 4) >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i]);
        terrainX0 = int(parseInteger(argv[i + 1], 10, "invalid terrain X0",
                                     -32768, 32767));
        terrainY0 = int(parseInteger(argv[i + 2], 10, "invalid terrain Y0",
                                     -32768, 32767));
        terrainX1 = int(parseInteger(argv[i + 3], 10, "invalid terrain X1",
                                     terrainX0, 32767));
        terrainY1 = int(parseInteger(argv[i + 4], 10, "invalid terrain Y1",
                                     terrainY0, 32767));
        i = i + 4;
      }
      else if (std::strcmp(argv[i], "-l") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        btdLOD = int(parseInteger(argv[i], 10,
                                  "invalid terrain level of detail", 0, 4));
      }
      else if (std::strcmp(argv[i], "-deftxt") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        defTxtID = (unsigned int) parseInteger(argv[i], 0, "invalid form ID",
                                               0, 0x0FFFFFFF);
      }
      else if (std::strcmp(argv[i], "-defclr") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        ltxtDefColor = (std::uint32_t) parseInteger(
                           argv[i], 0, "invalid land texture color",
                           0, 0x00FFFFFF);
      }
      else if (std::strcmp(argv[i], "-ltxtres") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        ltxtResolution = int(parseInteger(argv[i], 0,
                                          "invalid land texture resolution",
                                          8, 4096));
        if (ltxtResolution & (ltxtResolution - 1))
          errorMessage("invalid land texture resolution");
      }
      else if (std::strcmp(argv[i], "-mip") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        textureMip = int(parseInteger(argv[i], 10, "invalid texture mip level",
                                      0, 15));
      }
      else if (std::strcmp(argv[i], "-lmip") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        landTextureMip = float(parseFloat(argv[i],
                                          "invalid land texture mip level",
                                          0.0, 15.0));
      }
      else if (std::strcmp(argv[i], "-lmult") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        landTextureMult = float(parseFloat(argv[i],
                                           "invalid land texture RGB scale",
                                           0.5, 8.0));
      }
      else if (std::strcmp(argv[i], "-view") == 0 ||
               std::strcmp(argv[i], "-cam") == 0)
      {
        if ((i + 7) >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i]);
        viewScale = float(parseFloat(argv[i + 1], "invalid view scale",
                                     1.0 / 512.0, 16.0));
        viewRotationX = float(parseFloat(argv[i + 2], "invalid view X rotation",
                                         -360.0, 360.0));
        viewRotationY = float(parseFloat(argv[i + 3], "invalid view Y rotation",
                                         -360.0, 360.0));
        viewRotationZ = float(parseFloat(argv[i + 4], "invalid view Z rotation",
                                         -360.0, 360.0));
        viewOffsX = float(parseFloat(argv[i + 5], "invalid view X offset",
                                     -1048576.0, 1048576.0));
        viewOffsY = float(parseFloat(argv[i + 6], "invalid view Y offset",
                                     -1048576.0, 1048576.0));
        viewOffsZ = float(parseFloat(argv[i + 7], "invalid view Z offset",
                                     -1048576.0, 1048576.0));
        if (argv[i][1] == 'c')
        {
          NIFFile::NIFVertexTransform vt(
              viewScale,
              viewRotationX * d, viewRotationY * d, viewRotationZ * d,
              0.0f, 0.0f, 0.0f);
          viewOffsX = -viewOffsX;
          viewOffsY = -viewOffsY;
          viewOffsZ = -viewOffsZ;
          vt.transformXYZ(viewOffsX, viewOffsY, viewOffsZ);
          if (verboseMode)
          {
            std::fprintf(stderr, "View offset: %.2f, %.2f, %.2f\n",
                         viewOffsX, viewOffsY, viewOffsZ);
          }
        }
        i = i + 7;
      }
      else if (std::strcmp(argv[i], "-zrange") == 0)
      {
        if ((i + 2) >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i]);
        zMin = int(parseInteger(argv[i + 1], 10, "invalid Z range",
                                0, 1048575));
        zMax = int(parseInteger(argv[i + 2], 10, "invalid Z range",
                                zMin + 1, 16777216));
        i = i + 2;
      }
      else if (std::strcmp(argv[i], "-light") == 0)
      {
        if ((i + 3) >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i]);
        rgbScale = float(parseFloat(argv[i + 1], "invalid RGB scale",
                                    0.125, 4.0));
        lightRotationY = float(parseFloat(argv[i + 2],
                                          "invalid light Y rotation",
                                          -360.0, 360.0));
        lightRotationZ = float(parseFloat(argv[i + 3],
                                          "invalid light Z rotation",
                                          -360.0, 360.0));
        i = i + 3;
      }
      else if (std::strcmp(argv[i], "-lcolor") == 0)
      {
        if ((i + 5) >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i]);
        lightLevel = float(parseFloat(argv[i + 1], "invalid light source level",
                                      0.125, 4.0));
        lightColor = int(parseInteger(argv[i + 2], 0,
                                      "invalid light source color",
                                      -1, 0x00FFFFFF)) & 0x00FFFFFF;
        envLevel = float(parseFloat(argv[i + 3],
                                    "invalid environment light level",
                                    0.125, 4.0));
        envColor = int(parseInteger(argv[i + 4], 0,
                                    "invalid environment light color",
                                    -1, 0x00FFFFFF)) & 0x00FFFFFF;
        ambientColor = int(parseInteger(argv[i + 5], 0, "invalid ambient light",
                                        -1, 0x00FFFFFF));
        i = i + 5;
      }
      else if (std::strcmp(argv[i], "-rscale") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        reflZScale =
            float(parseFloat(argv[i], "invalid reflection view vector Z scale",
                             0.25, 16.0));
      }
      else if (std::strcmp(argv[i], "-mlod") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        modelLOD = int(parseInteger(argv[i], 10, "invalid model LOD", 0, 4));
      }
      else if (std::strcmp(argv[i], "-vis") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        distantObjectsOnly =
            bool(parseInteger(argv[i], 0, "invalid argument for -vis", 0, 1));
      }
      else if (std::strcmp(argv[i], "-ndis") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        noDisabledObjects =
            bool(parseInteger(argv[i], 0, "invalid argument for -ndis", 0, 1));
      }
      else if (std::strcmp(argv[i], "-hqm") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        hdModelNamePatterns.push_back(argv[i]);
      }
      else if (std::strcmp(argv[i], "-xm") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        excludeModelPatterns.push_back(argv[i]);
      }
      else if (std::strcmp(argv[i], "-env") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        defaultEnvMap = argv[i];
      }
      else if (std::strcmp(argv[i], "-wtxt") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        waterTexture = argv[i];
      }
      else if (std::strcmp(argv[i], "-watercolor") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        waterColor =
            std::uint32_t(parseInteger(argv[i], 0, "invalid water color",
                                       0, 0x7FFFFFFF));
      }
      else if (std::strcmp(argv[i], "-wrefl") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        waterReflectionLevel =
            float(parseFloat(argv[i], "invalid water environment map scale",
                             0.0, 2.0));
      }
      else if (std::strcmp(argv[i], "-wscale") == 0)
      {
        if (++i >= argc)
          throw FO76UtilsError("missing argument for %s", argv[i - 1]);
        waterUVScale =
            int(parseInteger(argv[i], 0, "invalid water texture tile size",
                             2, 32768));
      }
      else
      {
        throw FO76UtilsError("invalid option: %s", argv[i]);
      }
    }
    if (args.size() != 5)
    {
      for (size_t i = 0; usageStrings[i]; i++)
        std::fprintf(stderr, "%s\n", usageStrings[i]);
      return err;
    }
    if (!(btdPath && *btdPath != '\0'))
    {
      btdPath = (char *) 0;
      btdLOD = 2;
    }
    if (debugMode == 1)
    {
      enableDownscale = false;
      enableSCOL = true;
      ltxtResolution = 128 >> btdLOD;
    }
    if (!formID)
      formID = (!btdPath ? 0x0000003CU : 0x0025DA15U);
    waterColor =
        waterColor + (waterColor & 0x7F000000U) + ((waterColor >> 30) << 24);
    if (!(waterColor & 0xFF000000U))
      waterColor = 0U;
    int     width =
        int(parseInteger(args[2], 0, "invalid image width", 2, 32768));
    int     height =
        int(parseInteger(args[3], 0, "invalid image height", 2, 32768));
    viewOffsX = viewOffsX + (float(width) * 0.5f);
    viewOffsY = viewOffsY + (float(height - 2) * 0.5f);
    viewOffsZ = viewOffsZ - float(zMin);
    zMax = zMax - zMin;
    if (enableDownscale)
    {
      width = width << 1;
      height = height << 1;
      viewScale = viewScale * 2.0f;
      viewOffsX = viewOffsX * 2.0f;
      viewOffsY = viewOffsY * 2.0f;
      viewOffsZ = viewOffsZ * 2.0f;
      zMax = zMax << 1;
      zMax = (zMax < 16777216 ? zMax : 16777216);
    }

    BA2File ba2File(args[4]);
    ESMFile esmFile(args[0]);
    unsigned int  worldID = Renderer::findParentWorld(esmFile, formID);
    if (worldID == 0xFFFFFFFFU)
      errorMessage("form ID not found in ESM, or invalid record type");

    Renderer  renderer(width, height, ba2File, esmFile,
                       (std::uint32_t *) 0, (float *) 0, zMax);
    if (threadCnt > 0)
      renderer.setThreadCount(threadCnt);
    renderer.setTextureCacheSize(size_t(textureCacheSize) << 20);
    renderer.setVerboseMode(verboseMode);
    renderer.setDistantObjectsOnly(distantObjectsOnly);
    renderer.setNoDisabledObjects(noDisabledObjects);
    renderer.setEnableSCOL(enableSCOL);
    renderer.setEnableAllObjects(enableAllObjects);
    renderer.setEnableTextures(enableTextures);
    renderer.setDebugMode(debugMode);
    renderer.setLandDefaultColor(ltxtDefColor);
    renderer.setLandTxtResolution(ltxtResolution);
    renderer.setTextureMipLevel(textureMip);
    renderer.setLandTextureMip(landTextureMip);
    renderer.setLandTxtRGBScale(landTextureMult);
    renderer.setModelLOD(modelLOD);
    renderer.setWaterColor(waterColor);
    renderer.setWaterEnvMapScale(waterReflectionLevel);
    renderer.setViewTransform(
        viewScale, viewRotationX * d, viewRotationY * d, viewRotationZ * d,
        viewOffsX, viewOffsY, viewOffsZ);
    renderer.setLightDirection(lightRotationY * d, lightRotationZ * d);
    if (defaultEnvMap && *defaultEnvMap)
      renderer.setDefaultEnvMap(std::string(defaultEnvMap));
    if (waterColor && waterTexture && *waterTexture)
      renderer.setWaterTexture(std::string(waterTexture));
    renderer.setRenderParameters(
        lightColor, ambientColor, envColor, lightLevel, envLevel, rgbScale,
        reflZScale, waterUVScale);

    for (size_t i = 0; i < hdModelNamePatterns.size(); i++)
    {
      if (hdModelNamePatterns[i] && hdModelNamePatterns[i][0])
        renderer.addHDModelPattern(std::string(hdModelNamePatterns[i]));
    }
    for (size_t i = 0; i < excludeModelPatterns.size(); i++)
    {
      if (excludeModelPatterns[i] && excludeModelPatterns[i][0])
        renderer.addExcludeModelPattern(std::string(excludeModelPatterns[i]));
    }

    if (worldID)
    {
      renderer.loadTerrain(btdPath, worldID, defTxtID,
                           btdLOD, terrainX0, terrainY0, terrainX1, terrainY1);
      renderer.renderTerrain(worldID);
      renderer.clear();
    }
    renderer.renderObjects(formID);
    if (verboseMode)
    {
      const NIFFile::NIFBounds& b = renderer.getBounds();
      if (b.xMax() > b.xMin())
      {
        float   scale = (!enableDownscale ? 1.0f : 0.5f);
        std::fprintf(stderr,
                     "Bounds: %6.0f, %6.0f, %6.0f to %6.0f, %6.0f, %6.0f\n",
                     b.xMin() * scale, b.yMin() * scale, b.zMin() * scale,
                     b.xMax() * scale, b.yMax() * scale, b.zMax() * scale);
      }
    }
    renderer.clear();
    renderer.deallocateBuffers(0x02);

    width = width >> int(enableDownscale);
    height = height >> int(enableDownscale);
    const std::uint32_t *imageDataPtr = renderer.getImageData();
    size_t  imageDataSize = size_t(width) * size_t(height);
    std::vector< std::uint32_t >  downsampleBuf;
    if (enableDownscale)
    {
      downsampleBuf.resize(imageDataSize);
      downsample2xFilter(
          &(downsampleBuf.front()), imageDataPtr, width << 1, height << 1,
          width, (unsigned char) ((outputFormat & 2) | USE_PIXELFMT_RGB10A2));
      imageDataPtr = &(downsampleBuf.front());
    }
    if (!outputFormat)
      outputFormat = DDSInputFile::pixelFormatRGB24;
    else if (outputFormat == 1)
      outputFormat = DDSInputFile::pixelFormatRGBA32;
    else
      outputFormat = DDSInputFile::pixelFormatA2R10G10B10;
    DDSOutputFile outFile(args[1], width, height, outputFormat);
    outFile.writeImageData(imageDataPtr, imageDataSize,
                           outputFormat, outputFormat);
    err = 0;
  }
  catch (std::exception& e)
  {
    std::fprintf(stderr, "render: %s\n", e.what());
    err = 1;
  }
  return err;
}


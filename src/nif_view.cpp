
#include "common.hpp"
#include "nif_view.hpp"
#include "downsamp.hpp"

#include <algorithm>
#ifdef HAVE_SDL2
#  include <SDL2/SDL.h>
#endif

#include "viewrtbl.cpp"

static const std::uint32_t  defaultWaterColor = 0xC0804000U;

static const char *cubeMapPaths[24] =
{
  "textures/cubemaps/bleakfallscube_e.dds",                     // Skyrim
  "textures/shared/cubemaps/mipblur_defaultoutside1.dds",       // Fallout 4
  "textures/shared/cubemaps/mipblur_defaultoutside1.dds",       // Fallout 76
  "textures/cubemaps/wrtemple_e.dds",
  "textures/shared/cubemaps/outsideoldtownreflectcube_e.dds",
  "textures/shared/cubemaps/outsideoldtownreflectcube_e.dds",
  "textures/cubemaps/duncaveruingreen_e.dds",
  "textures/shared/cubemaps/cgprewarstreet_e.dds",
  "textures/shared/cubemaps/swampcube.dds",
  "textures/cubemaps/chrome_e.dds",
  "textures/shared/cubemaps/metalchrome01cube_e.dds",
  "textures/shared/cubemaps/metalchrome01cube_e.dds",
  "textures/cubemaps/cavegreencube_e.dds",
  "textures/shared/cubemaps/outsideday01.dds",
  "textures/shared/cubemaps/outsideday01.dds",
  "textures/cubemaps/mghallcube_e.dds",
  "textures/shared/cubemaps/cgplayerhousecube.dds",
  "textures/shared/cubemaps/chrome_e.dds",
  "textures/cubemaps/caveicecubemap_e.dds",
  "textures/shared/cubemaps/inssynthproductionpoolcube.dds",
  "textures/shared/cubemaps/vault111cryocube.dds",
  "textures/cubemaps/minecube_e.dds",
  "textures/shared/cubemaps/memorydencube.dds",
  "textures/shared/cubemaps/mipblur_defaultoutside_pitt.dds"
};

const char * NIF_View::materialFlagNames[32] =
{
  "tile U", "tile V", "is effect", "decal", "two sided", "tree",
  "grayscale to alpha", "glow", "no Z buffer write", "falloff enabled",
  "effect lighting", "ordered with previous", "alpha blending",
  "has vertex colors", "is water", "hidden",
  "is marker", "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""
};

void NIF_View::threadFunction(NIF_View *p, size_t n)
{
  p->threadErrMsg[n].clear();
  try
  {
    std::vector< TriShapeSortObject > sortBuf;
    sortBuf.reserve(p->meshData.size());
    NIFFile::NIFVertexTransform mt(p->modelTransform);
    NIFFile::NIFVertexTransform vt(p->viewTransform);
    mt *= vt;
    vt.offsY = vt.offsY - float(p->viewOffsetY[n]);
    p->renderers[n]->setViewAndLightVector(vt, p->lightX, p->lightY, p->lightZ);
    for (size_t i = 0; i < p->meshData.size(); i++)
    {
      const NIFFile::NIFTriShape& ts = p->meshData[i];
      if (!p->enableHidden)
      {
        // ignore if hidden or marker
        if (ts.m.flags & (BGSMFile::Flag_TSHidden | BGSMFile::Flag_TSMarker))
          continue;
      }
      NIFFile::NIFBounds  b;
      ts.calculateBounds(b, &mt);
      if (roundFloat(b.xMax()) < 0 ||
          roundFloat(b.yMin()) > p->viewOffsetY[n + 1] ||
          roundFloat(b.yMax()) < p->viewOffsetY[n] ||
          b.zMax() < 0.0f)
      {
        continue;
      }
      sortBuf.emplace(sortBuf.end(), i, b.zMin(),
                      bool(ts.m.flags & BGSMFile::Flag_TSAlphaBlending));
      if (ts.m.flags & BGSMFile::Flag_TSOrdered) [[unlikely]]
        TriShapeSortObject::orderedNodeFix(sortBuf, p->meshData);
    }
    if (sortBuf.size() < 1)
      return;
    std::sort(sortBuf.begin(), sortBuf.end());
    for (size_t i = 0; i < sortBuf.size(); i++)
    {
      Plot3D_TriShape&  ts = *(p->renderers[n]);
      {
        size_t  j = size_t(sortBuf[i]);
        ts = p->meshData[j];
        if (p->debugMode == 1) [[unlikely]]
        {
          std::uint32_t c = std::uint32_t(j + 1);
          c = ((c & 1U) << 23) | ((c & 2U) << 14) | ((c & 4U) << 5)
              | ((c & 8U) << 19) | ((c & 16U) << 10) | ((c & 32U) << 1)
              | ((c & 64U) << 15) | ((c & 128U) << 6) | ((c & 256U) >> 3)
              | ((c & 512U) << 11) | ((c & 1024U) << 2) | ((c & 2048U) >> 7);
          c = c ^ 0xFFFFFFFFU;
          ts.setDebugMode(1, c);
        }
      }
      const DDSTexture  *textures[10];
      unsigned int  textureMask = 0U;
      if (ts.m.flags & BGSMFile::Flag_TSWater) [[unlikely]]
      {
        if (bool(textures[1] = p->loadTexture(p->waterTexture, n)))
          textureMask |= 0x0002U;
        if (bool(textures[4] = p->loadTexture(p->defaultEnvMap, n)))
          textureMask |= 0x0010U;
        if (p->waterFormID)
          ts.setMaterial(p->waterMaterials[p->waterFormID]);
        else
          ts.m.setWaterColor(defaultWaterColor, p->waterEnvMapLevel);
      }
      else
      {
        for (size_t j = 0; p->materialSwapTable[j]; j++) [[unlikely]]
        {
          if (p->materialSwapTable[j] & 0x80000000U)
          {
            ts.m.s.gradientMapV = float(int(~(p->materialSwapTable[j])))
                                  * (1.0f / 16777216.0f);
          }
          else
          {
            p->materialSwaps.materialSwap(ts, p->materialSwapTable[j]);
          }
          if ((j + 1) >= (sizeof(p->materialSwapTable) / sizeof(unsigned int)))
            break;
        }
        unsigned int  texturePathMask =
            (!(ts.m.flags & BGSMFile::Flag_Glow) ? 0x037BU : 0x037FU)
            & (unsigned int) ts.m.texturePathMask;
        for (size_t j = 0; j < 10; j++, texturePathMask >>= 1)
        {
          if (texturePathMask & 1)
          {
            if (bool(textures[j] = p->loadTexture(ts.m.texturePaths[j], n)))
              textureMask |= (1U << (unsigned char) j);
          }
        }
        if (!(textureMask & 0x0001U))
        {
          textures[0] = &(p->defaultTexture);
          textureMask |= 0x0001U;
        }
        if (!(textureMask & 0x0010U) && ts.m.s.envMapScale > 0.0f)
        {
          if (bool(textures[4] = p->loadTexture(p->defaultEnvMap, n)))
            textureMask |= 0x0010U;
        }
      }
      ts.drawTriShape(p->modelTransform, textures, textureMask);
    }
  }
  catch (std::exception& e)
  {
    p->threadErrMsg[n] = e.what();
    if (p->threadErrMsg[n].empty())
      p->threadErrMsg[n] = "unknown error in render thread";
  }
}

const DDSTexture * NIF_View::loadTexture(const std::string& texturePath,
                                         size_t threadNum)
{
  const DDSTexture  *t =
      textureSet.loadTexture(ba2File, texturePath,
                             threadFileBuffers[threadNum], 0);
#if 0
  if (!t && !texturePath.empty())
  {
    std::fprintf(stderr, "Warning: failed to load texture '%s'\n",
                 texturePath.c_str());
  }
#endif
  return t;
}

void NIF_View::setDefaultTextures(int envMapNum)
{
  int     n = 0;
  if (nifFile && nifFile->getVersion() >= 0x80)
    n = (nifFile->getVersion() < 0x90 ? 1 : 2);
  n = n + ((envMapNum & 7) * 3);
  defaultEnvMap = cubeMapPaths[n];
}

NIF_View::NIF_View(const BA2File& archiveFiles, ESMFile *esmFilePtr)
  : ba2File(archiveFiles),
    esmFile(esmFilePtr),
    textureSet(0x10000000),
    lightX(0.0f),
    lightY(0.0f),
    lightZ(1.0f),
    nifFile(nullptr),
    defaultTexture(0xFFFFFFFFU),
    waterTexture("textures/water/defaultwater.dds"),
    modelRotationX(0.0f),
    modelRotationY(0.0f),
    modelRotationZ(0.0f),
    viewRotationX(54.73561f),
    viewRotationY(180.0f),
    viewRotationZ(45.0f),
    viewOffsX(0.0f),
    viewOffsY(0.0f),
    viewOffsZ(0.0f),
    viewScale(1.0f),
    lightRotationY(56.25f),
    lightRotationZ(-135.0f),
    lightColor(1.0f),
    envColor(1.0f),
    rgbScale(1.0f),
    reflZScale(1.0f),
    waterEnvMapLevel(1.0f),
    waterFormID(0U),
    enableHidden(false),
    debugMode(0)
{
  threadCnt = int(std::thread::hardware_concurrency());
  threadCnt = (threadCnt > 1 ? (threadCnt < 16 ? threadCnt : 16) : 1);
  renderers.resize(size_t(threadCnt), nullptr);
  threadErrMsg.resize(size_t(threadCnt));
  viewOffsetY.resize(size_t(threadCnt + 1), 0);
  threadFileBuffers.resize(size_t(threadCnt));
  clearMaterialSwaps();
  try
  {
    for (size_t i = 0; i < renderers.size(); i++)
      renderers[i] = new Plot3D_TriShape(nullptr, nullptr, 0, 0, 4U);
  }
  catch (...)
  {
    for (size_t i = 0; i < renderers.size(); i++)
    {
      if (renderers[i])
      {
        delete renderers[i];
        renderers[i] = nullptr;
      }
    }
    throw;
  }
}

NIF_View::~NIF_View()
{
  if (nifFile)
  {
    delete nifFile;
    nifFile = nullptr;
  }
  textureSet.clear();
  for (size_t i = 0; i < renderers.size(); i++)
  {
    if (renderers[i])
      delete renderers[i];
  }
}

void NIF_View::loadModel(const std::string& fileName)
{
  meshData.clear();
  textureSet.shrinkTextureCache();
  if (nifFile)
  {
    delete nifFile;
    nifFile = nullptr;
  }
  if (fileName.empty())
    return;
  bool    isMaterialFile =
      (fileName.starts_with("materials/") &&
       (fileName.ends_with(".bgsm") || fileName.ends_with(".bgem")));
  BA2File::UCharArray&  fileBuf = threadFileBuffers[0];
  if (isMaterialFile)
  {
    ba2File.extractFile(fileBuf,
                        std::string("meshes/preview/previewsphere01.nif"));
  }
  else
  {
    ba2File.extractFile(fileBuf, fileName);
  }
  nifFile = new NIFFile(fileBuf.data, fileBuf.size, &ba2File);
  try
  {
    nifFile->getMesh(meshData);
    if (isMaterialFile)
    {
      BGSMFile  bgsmFile(ba2File, fileName);
      for (size_t i = 0; i < meshData.size(); i++)
        meshData[i].setMaterial(bgsmFile);
    }
  }
  catch (...)
  {
    meshData.clear();
    delete nifFile;
    nifFile = nullptr;
    throw;
  }
}

static inline float degreesToRadians(float x)
{
  return float(double(x) * (std::atan(1.0) / 45.0));
}

void NIF_View::renderModel(std::uint32_t *outBufRGBA, float *outBufZ,
                           int imageWidth, int imageHeight)
{
  if (!(meshData.size() > 0 &&
        outBufRGBA && outBufZ && imageWidth > 0 && imageHeight >= threadCnt))
  {
    return;
  }
  unsigned int  renderMode = (nifFile->getVersion() < 0x80U ?
                              7U : (nifFile->getVersion() < 0x90U ? 11U : 15U));
  float   y0 = 0.0f;
  for (size_t i = 0; i < renderers.size(); i++)
  {
    int     y0i = roundFloat(y0);
    int     y1i = imageHeight;
    if ((i + 1) < renderers.size())
    {
      float   y1 = float(int(i + 1)) / float(int(renderers.size()));
      if (imageHeight >= 32)
        y1 = ((y1 * 2.0f - 3.0f) * y1 + 2.0f) * y1;
      y1 = y1 * float(imageHeight);
      y1i = roundFloat(y1);
      y0 = y1;
    }
    viewOffsetY[i] = y0i;
    size_t  offs = size_t(y0i) * size_t(imageWidth);
    renderers[i]->setRenderMode(renderMode);
    renderers[i]->setBuffers(outBufRGBA + offs, outBufZ + offs,
                             imageWidth, y1i - y0i);
    renderers[i]->setEnvMapOffset(float(imageWidth) * -0.5f,
                                  float(imageHeight) * -0.5f + float(y0i),
                                  float(imageHeight) * reflZScale);
  }
  viewOffsetY[renderers.size()] = imageHeight;
  if (defaultEnvMap.empty())
    setDefaultTextures();
  {
    FloatVector4  ambientLight =
        renderers[0]->cubeMapToAmbient(loadTexture(defaultEnvMap, 0));
    for (size_t i = 0; i < renderers.size(); i++)
    {
      renderers[i]->setLighting(lightColor, ambientLight, envColor, rgbScale);
      renderers[i]->setDebugMode((unsigned int) debugMode, 0);
    }
  }
  modelTransform = NIFFile::NIFVertexTransform(
                       1.0f, degreesToRadians(modelRotationX),
                       degreesToRadians(modelRotationY),
                       degreesToRadians(modelRotationZ), 0.0f, 0.0f, 0.0f);
  viewTransform = NIFFile::NIFVertexTransform(
                      1.0f, degreesToRadians(viewRotationX),
                      degreesToRadians(viewRotationY),
                      degreesToRadians(viewRotationZ), 0.0f, 0.0f, 0.0f);
  {
    NIFFile::NIFVertexTransform
        lightTransform(1.0f, 0.0f, degreesToRadians(lightRotationY),
                       degreesToRadians(lightRotationZ), 0.0f, 0.0f, 0.0f);
    lightX = lightTransform.rotateZX;
    lightY = lightTransform.rotateZY;
    lightZ = lightTransform.rotateZZ;
  }
  {
    NIFFile::NIFVertexTransform t(modelTransform);
    t *= viewTransform;
    NIFFile::NIFBounds  b;
    for (size_t i = 0; i < meshData.size(); i++)
    {
      if (!enableHidden)
      {
        // ignore if hidden or marker
        if (meshData[i].m.flags & (BGSMFile::Flag_TSHidden
                                   | BGSMFile::Flag_TSMarker))
        {
          continue;
        }
      }
      meshData[i].calculateBounds(b, &t);
    }
    float   xScale = float(imageWidth) * 0.96875f;
    if (b.xMax() > b.xMin())
      xScale = xScale / (b.xMax() - b.xMin());
    float   yScale = float(imageHeight) * 0.96875f;
    if (b.yMax() > b.yMin())
      yScale = yScale / (b.yMax() - b.yMin());
    float   scale = (xScale < yScale ? xScale : yScale) * viewScale;
    viewTransform.scale = scale;
    viewTransform.offsX = (float(imageWidth) - ((b.xMin() + b.xMax()) * scale))
                          * 0.5f + viewOffsX;
    viewTransform.offsY = (float(imageHeight) - ((b.yMin() + b.yMax()) * scale))
                          * 0.5f + viewOffsY;
    viewTransform.offsZ = viewOffsZ + 1.0f - (b.zMin() * scale);
  }
  std::vector< std::thread * >  threads(renderers.size(), nullptr);
  try
  {
    for (size_t i = 0; i < threads.size(); i++)
      threads[i] = new std::thread(threadFunction, this, i);
    for (size_t i = 0; i < threads.size(); i++)
    {
      if (threads[i])
      {
        threads[i]->join();
        delete threads[i];
        threads[i] = nullptr;
      }
      if (!threadErrMsg[i].empty())
        throw FO76UtilsError(1, threadErrMsg[i].c_str());
    }
  }
  catch (...)
  {
    for (size_t i = 0; i < threads.size(); i++)
    {
      if (threads[i])
      {
        threads[i]->join();
        delete threads[i];
      }
    }
    throw;
  }
}

void NIF_View::addMaterialSwap(unsigned int formID)
{
  if (!(formID & 0x80000000U))
  {
    unsigned int  n = formID;
    formID = 0U;
    if (n && esmFile)
      formID = materialSwaps.loadMaterialSwap(ba2File, *esmFile, n);
  }
  if (!formID)
    return;
  size_t  i;
  for (i = 0; i < (sizeof(materialSwapTable) / sizeof(unsigned int)); i++)
  {
    if (!materialSwapTable[i])
    {
      materialSwapTable[i] = formID;
      break;
    }
  }
}

void NIF_View::clearMaterialSwaps()
{
  size_t  i;
  for (i = 0; i < (sizeof(materialSwapTable) / sizeof(unsigned int)); i++)
    materialSwapTable[i] = 0U;
}

void NIF_View::setWaterColor(unsigned int watrFormID)
{
  if (watrFormID && esmFile)
  {
    const ESMFile::ESMRecord  *r = esmFile->findRecord(watrFormID);
    if (r && *r == "WATR")
    {
      waterFormID = getWaterMaterial(waterMaterials, *esmFile, r, 0U);
      return;
    }
  }
  waterFormID = 0U;
}

void NIF_View::renderModelToFile(const char *outFileName,
                                 int imageWidth, int imageHeight)
{
  size_t  imageDataSize = size_t(imageWidth << 1) * size_t(imageHeight << 1);
  std::vector< std::uint32_t >  outBufRGBA(imageDataSize, 0U);
  std::vector< float >  outBufZ(imageDataSize, 16777216.0f);
  renderModel(outBufRGBA.data(), outBufZ.data(),
              imageWidth << 1, imageHeight << 1);
  imageDataSize = imageDataSize >> 2;
  std::vector< std::uint32_t >  downsampleBuf(imageDataSize);
  downsample2xFilter(downsampleBuf.data(), outBufRGBA.data(),
                     imageWidth << 1, imageHeight << 1, imageWidth);
  int     pixelFormat =
      (!USE_PIXELFMT_RGB10A2 ?
       DDSInputFile::pixelFormatRGBA32 : DDSInputFile::pixelFormatA2R10G10B10);
  DDSOutputFile outFile(outFileName, imageWidth, imageHeight, pixelFormat);
  outFile.writeImageData(downsampleBuf.data(), imageDataSize, pixelFormat);
}

#ifdef HAVE_SDL2
static void updateRotation(float& rx, float& ry, float& rz,
                           int dx, int dy, int dz,
                           std::string& messageBuf, const char *msg)
{
  rx += (float(dx) * 2.8125f);
  ry += (float(dy) * 2.8125f);
  rz += (float(dz) * 2.8125f);
  rx = (rx < -180.0f ? (rx + 360.0f) : (rx > 180.0f ? (rx - 360.0f) : rx));
  ry = (ry < -180.0f ? (ry + 360.0f) : (ry > 180.0f ? (ry - 360.0f) : ry));
  rz = (rz < -180.0f ? (rz + 360.0f) : (rz > 180.0f ? (rz - 360.0f) : rz));
  if (!msg)
    return;
  char    buf[64];
  std::snprintf(buf, 64, "%s %7.2f %7.2f %7.2f\n", msg, rx, ry, rz);
  buf[63] = '\0';
  messageBuf = buf;
}

static void updateLightColor(FloatVector4& lightColor, int dR, int dG, int dB,
                             std::string& messageBuf)
{
  int     r = roundFloat(float(std::log2(lightColor[0])) * 16.0f);
  lightColor[0] = float(std::exp2(float(r + dR) * 0.0625f));
  int     g = roundFloat(float(std::log2(lightColor[1])) * 16.0f);
  lightColor[1] = float(std::exp2(float(g + dG) * 0.0625f));
  int     b = roundFloat(float(std::log2(lightColor[2])) * 16.0f);
  lightColor[2] = float(std::exp2(float(b + dB) * 0.0625f));
  lightColor.maxValues(FloatVector4(0.0625f));
  lightColor.minValues(FloatVector4(4.0f));
  char    buf[64];
  std::snprintf(buf, 64,
                "Light color (linear color space): %7.4f %7.4f %7.4f\n",
                lightColor[0], lightColor[1], lightColor[2]);
  buf[63] = '\0';
  messageBuf = buf;
}

static void updateValueLogScale(float& s, int d, float minVal, float maxVal,
                                std::string& messageBuf, const char *msg)
{
  int     tmp = roundFloat(float(std::log2(s)) * 16.0f);
  s = float(std::exp2(float(tmp + d) * 0.0625f));
  s = (s > minVal ? (s < maxVal ? s : maxVal) : minVal);
  if (!msg)
    return;
  char    buf[64];
  std::snprintf(buf, 64, "%s: %7.4f\n", msg, s);
  buf[63] = '\0';
  messageBuf = buf;
}

static void printViewScale(SDLDisplay& display, float viewScale,
                           const NIFFile::NIFVertexTransform& vt)
{
  float   z = (vt.rotateXX * vt.rotateXX) + (vt.rotateYX * vt.rotateYX);
  z = std::max(z, (vt.rotateXY * vt.rotateXY) + (vt.rotateYY * vt.rotateYY));
  z = std::max(z, (vt.rotateXZ * vt.rotateXZ) + (vt.rotateYZ * vt.rotateYZ));
  z = float(std::sqrt(std::max(z, 0.25f)));
  float   vtScale = vt.scale * z / float(1 << display.getDownsampleLevel());
  char    buf[256];
  std::snprintf(buf, 256, "View scale: %7.4f (1 unit = %7.4f pixels)\n",
                viewScale, vtScale);
  buf[255] = '\0';
  display.printString(buf);
}

static void saveScreenshot(SDLDisplay& display, const std::string& nifFileName,
                           NIF_View *renderer = nullptr)
{
  size_t  n1 = nifFileName.rfind('/');
  n1 = (n1 != std::string::npos ? (n1 + 1) : 0);
  size_t  n2 = nifFileName.rfind('.');
  n2 = (n2 != std::string::npos ? n2 : 0);
  std::string fileName;
  if (n2 > n1)
    fileName.assign(nifFileName, n1, n2 - n1);
  else
    fileName = "nif_info";
  std::time_t t = std::time(nullptr);
  {
    unsigned int  s = (unsigned int) (t % (std::time_t) (24 * 60 * 60));
    unsigned int  m = s / 60U;
    s = s % 60U;
    unsigned int  h = m / 60U;
    m = m % 60U;
    h = h % 24U;
    char    buf[16];
    std::sprintf(buf, "_%02u%02u%02u.dds", h, m, s);
    fileName += buf;
  }
  int     w = display.getWidth() >> display.getDownsampleLevel();
  int     h = display.getHeight() >> display.getDownsampleLevel();
  if (!renderer)
  {
    display.blitSurface();
    const std::uint32_t *p = display.lockScreenSurface();
    DDSOutputFile f(fileName.c_str(), w, h, DDSInputFile::pixelFormatRGBA32);
    size_t  pitch = display.getPitch();
    for (int y = 0; y < h; y++, p = p + pitch)
      f.writeImageData(p, size_t(w), DDSInputFile::pixelFormatRGBA32);
    display.unlockScreenSurface();
  }
  else
  {
    renderer->renderModelToFile(fileName.c_str(), w << 1, h << 1);
  }
  display.printString("Saved screenshot to ");
  display.printString(fileName.c_str());
  display.printString("\n");
}

static void printMaterialInfo(SDLDisplay& display, const BGSMFile& bgsmFile)
{
  display.consolePrint("    Material version: %2u\n",
                       (unsigned int) bgsmFile.version);
  if (bgsmFile.flags)
  {
    display.consolePrint("    Material flags: ");
    for (unsigned int i = 0U; i < 32U; i++)
    {
      if (!(bgsmFile.flags & (1U << i)))
        continue;
      if (!(bgsmFile.flags & ((1U << i) - 1U)))
        display.consolePrint("%s", NIF_View::materialFlagNames[i]);
      else
        display.consolePrint(", %s", NIF_View::materialFlagNames[i]);
    }
    display.consolePrint("\n");
  }
  display.consolePrint("    Material alpha flags: 0x%04X\n",
                       (unsigned int) bgsmFile.alphaFlags);
  display.consolePrint("    Material alpha threshold: %3u (%.3f)\n",
                       (unsigned int) bgsmFile.alphaThreshold,
                       bgsmFile.alphaThresholdFloat);
  display.consolePrint("    Material alpha: %.3f\n", bgsmFile.alpha);
  display.consolePrint("    Material texture U, V offset: %.3f, %.3f\n",
                       bgsmFile.textureOffsetU, bgsmFile.textureOffsetV);
  display.consolePrint("    Material texture U, V scale: %.3f, %.3f\n",
                       bgsmFile.textureScaleU, bgsmFile.textureScaleV);
  if (bgsmFile.flags & BGSMFile::Flag_TSWater)
  {
    unsigned int  shallowColor =
        std::uint32_t(bgsmFile.w.shallowColor * 255.0f);
    unsigned int  deepColor = std::uint32_t(bgsmFile.w.deepColor * 255.0f);
    display.consolePrint("    Water shallow color (0xAABBGGRR): 0x%08X\n",
                         shallowColor);
    display.consolePrint("    Water deep color (0xAABBGGRR): 0x%08X\n",
                         deepColor);
    display.consolePrint("    Water depth range: %.3f\n", bgsmFile.w.maxDepth);
    display.consolePrint("    Water environment map scale: %.3f\n",
                         bgsmFile.w.envMapScale);
    display.consolePrint("    Water specular smoothness: %.3f\n",
                         bgsmFile.w.specularSmoothness);
  }
  else if (bgsmFile.flags & BGSMFile::Flag_IsEffect)
  {
    unsigned int  baseColor = std::uint32_t(bgsmFile.e.baseColor * 255.0f);
    display.consolePrint("    Effect base color (0xAABBGGRR): 0x%08X\n",
                         baseColor);
    display.consolePrint("    Effect base color scale: %.3f\n",
                         bgsmFile.e.baseColorScale);
    display.consolePrint("    Effect lighting influence: %.3f\n",
                         bgsmFile.e.lightingInfluence);
    display.consolePrint("    Effect environment map scale: %.3f\n",
                         bgsmFile.e.envMapScale);
    display.consolePrint("    Effect specular smoothness: %.3f\n",
                         bgsmFile.e.specularSmoothness);
    display.consolePrint("    Effect falloff parameters: "
                         "%.3f, %.3f, %.3f, %.3f\n",
                         bgsmFile.e.falloffParams[0],
                         bgsmFile.e.falloffParams[1],
                         bgsmFile.e.falloffParams[2],
                         bgsmFile.e.falloffParams[3]);
  }
  else
  {
    display.consolePrint("    Material gradient map scale: %.3f\n",
                         bgsmFile.s.gradientMapV);
    display.consolePrint("    Material environment map scale: %.3f\n",
                         bgsmFile.s.envMapScale);
    unsigned int  specularColor =
        std::uint32_t(bgsmFile.s.specularColor * 255.0f);
    unsigned int  emissiveColor =
        std::uint32_t(bgsmFile.s.emissiveColor * 255.0f);
    display.consolePrint("    Material specular color (0xBBGGRR): 0x%06X\n",
                         specularColor & 0x00FFFFFFU);
    display.consolePrint("    Material specular scale: %.3f\n",
                         bgsmFile.s.specularColor[3]);
    display.consolePrint("    Material specular smoothness: %.3f\n",
                         bgsmFile.s.specularSmoothness);
    display.consolePrint("    Material emissive color (0xBBGGRR): 0x%06X\n",
                         emissiveColor & 0x00FFFFFFU);
    display.consolePrint("    Material emissive scale: %.3f\n",
                         bgsmFile.s.emissiveColor[3]);
  }
  unsigned int  m = bgsmFile.texturePathMask;
  for (size_t i = 0; m; i++, m = m >> 1)
  {
    if (!(m & 1U))
      continue;
    display.consolePrint("    Material texture %d: %s\n",
                         int(i), bgsmFile.texturePaths[i].c_str());
  }
}

static bool viewModelInfo(SDLDisplay& display, const NIFFile& nifFile)
{
  display.clearTextBuffer();
  display.consolePrint("BS version: 0x%08X\n", nifFile.getVersion());
  display.consolePrint("Author name: %s\n", nifFile.getAuthorName().c_str());
  display.consolePrint("Process script: %s\n",
                       nifFile.getProcessScriptName().c_str());
  display.consolePrint("Export script: %s\n",
                       nifFile.getExportScriptName().c_str());
  display.consolePrint("Block count: %u\n",
                       (unsigned int) nifFile.getBlockCount());
  for (size_t i = 0; i < nifFile.getBlockCount(); i++)
  {
    display.consolePrint("  Block %3d: offset = 0x%08X, size = %7u, "
                         "name = \"%s\", type = %s\n",
                         int(i),
                         (unsigned int) nifFile.getBlockOffset(i),
                         (unsigned int) nifFile.getBlockSize(i),
                         nifFile.getBlockName(i),
                         nifFile.getBlockTypeAsString(i));
    const NIFFile::NIFBlkNiNode *nodeBlock = nifFile.getNode(i);
    const NIFFile::NIFBlkBSTriShape *triShapeBlock = nifFile.getTriShape(i);
    const NIFFile::NIFBlkBSLightingShaderProperty *
        lspBlock = nifFile.getLightingShaderProperty(i);
    const NIFFile::NIFBlkBSShaderTextureSet *
        tsBlock = nifFile.getShaderTextureSet(i);
    const NIFFile::NIFBlkNiAlphaProperty *
        alphaPropertyBlock = nifFile.getAlphaProperty(i);
    if (nodeBlock)
    {
      if (nodeBlock->controller >= 0)
        display.consolePrint("    Controller: %3d\n", nodeBlock->controller);
      if (nodeBlock->collisionObject >= 0)
      {
        display.consolePrint("    Collision object: %3d\n",
                             nodeBlock->collisionObject);
      }
      if (nodeBlock->children.size() > 0)
      {
        display.consolePrint("    Children: ");
        for (size_t j = 0; j < nodeBlock->children.size(); j++)
        {
          display.consolePrint("%s%3u", (!j ? "" : ", "),
                               nodeBlock->children[j]);
        }
        display.consolePrint("\n");
      }
    }
    else if (triShapeBlock)
    {
      if (triShapeBlock->controller >= 0)
      {
        display.consolePrint("    Controller: %3d\n",
                             triShapeBlock->controller);
      }
      display.consolePrint("    Flags: 0x%08X\n", triShapeBlock->flags);
      if (triShapeBlock->collisionObject >= 0)
      {
        display.consolePrint("    Collision object: %3d\n",
                             triShapeBlock->collisionObject);
      }
      if (triShapeBlock->skinID >= 0)
        display.consolePrint("    Skin: %3d\n", triShapeBlock->skinID);
      if (triShapeBlock->shaderProperty >= 0)
      {
        display.consolePrint("    Shader property: %3d\n",
                             triShapeBlock->shaderProperty);
      }
      if (triShapeBlock->alphaProperty >= 0)
      {
        display.consolePrint("    Alpha property: %3d\n",
                             triShapeBlock->alphaProperty);
      }
      display.consolePrint("    Vertex count: %lu\n",
                           (unsigned long) triShapeBlock->vertexData.size());
      display.consolePrint("    Triangle count: %lu\n",
                           (unsigned long) triShapeBlock->triangleData.size());
    }
    else if (lspBlock)
    {
      if (lspBlock->controller >= 0)
        display.consolePrint("    Controller: %3d\n", lspBlock->controller);
      if (lspBlock->material.version < 20)
      {
        display.consolePrint("    Flags: 0x%016llX\n",
                             (unsigned long long) lspBlock->flags);
      }
      if (lspBlock->textureSet >= 0)
        display.consolePrint("    Texture set: %3d\n", lspBlock->textureSet);
      printMaterialInfo(display, lspBlock->material);
    }
    else if (tsBlock)
    {
      unsigned int  m = tsBlock->texturePathMask;
      for (size_t j = 0; m; j++, m = m >> 1)
      {
        if (!(m & 1U))
          continue;
        display.consolePrint("    Texture %2d: %s\n",
                             int(j), tsBlock->texturePaths[j].c_str());
      }
    }
    else if (alphaPropertyBlock)
    {
      if (alphaPropertyBlock->controller >= 0)
      {
        display.consolePrint("    Controller: %3d\n",
                             alphaPropertyBlock->controller);
      }
      display.consolePrint("    Flags: 0x%04X\n",
                           (unsigned int) alphaPropertyBlock->flags);
      display.consolePrint("    Alpha threshold: %u\n",
                           (unsigned int) alphaPropertyBlock->alphaThreshold);
    }
  }
  return display.viewTextBuffer();
}

static bool viewMaterialInfo(
    SDLDisplay& display, const BA2File& ba2File, const std::string& fileName)
{
  display.clearTextBuffer();
  display.consolePrint("Material path: \"%s\"\n", fileName.c_str());
  try
  {
    BGSMFile  bgsmFile(ba2File, fileName);
    printMaterialInfo(display, bgsmFile);
    return display.viewTextBuffer();
  }
  catch (FO76UtilsError&)
  {
    display.consolePrint("Not found in database\n");
  }
  return true;
}

static bool printGeometryBlockInfo(
    SDLDisplay& display, int x, int y, int mouseButton,
    const NIFFile *nifFile, const std::vector< NIFFile::NIFTriShape >& meshData)
{
  std::uint32_t c =
      display.lockDrawSurface()[size_t(y) * size_t(display.getWidth())
                                + size_t(x)];
  display.unlockDrawSurface();
#if USE_PIXELFMT_RGB10A2
  c = ((c >> 2) & 0xFFU) | ((c >> 4) & 0xFF00U) | ((c >> 6) & 0xFF0000U);
#else
  c = ((c & 0xFFU) << 16) | ((c >> 16) & 0xFFU) | (c & 0xFF00U);
#endif
  c = c ^ 0xFFFFFFFFU;
  c = ((c >> 23) & 0x0001U) | ((c >> 14) & 0x0002U) | ((c >> 5) & 0x0004U)
      | ((c >> 19) & 0x0008U) | ((c >> 10) & 0x0010U) | ((c >> 1) & 0x0020U)
      | ((c >> 15) & 0x0040U) | ((c >> 6) & 0x0080U) | ((c << 3) & 0x0100U)
      | ((c >> 11) & 0x0200U) | ((c >> 2) & 0x0400U) | ((c << 7) & 0x0800U)
      | ((c >> 7) & 0x1000U) | ((c << 2) & 0x2000U) | ((c << 11) & 0x4000U);
  if (!c || c > meshData.size() || !nifFile)
    return false;
  c--;
  std::string tmpBuf;
  const NIFFile::NIFTriShape& ts = meshData[c];
  const std::string *blkName = nifFile->getString(ts.nameID);
  if (blkName && !blkName->empty())
    printToString(tmpBuf, "BSTriShape: \"%s\"\n", blkName->c_str());
  if (ts.haveMaterialPath())
    printToString(tmpBuf, "Material path: \"%s\"\n", ts.materialPath().c_str());
  if (mouseButton == 2)
  {
    printToString(tmpBuf, "Vertex count: %u\n", ts.vertexCnt);
    printToString(tmpBuf, "Triangle count: %u\n", ts.triangleCnt);
    printToString(tmpBuf, "Vertex data:\n");
    for (size_t i = 0; i < ts.vertexCnt; i++)
    {
      NIFFile::NIFVertex  v(ts.vertexData[i]);
      FloatVector4  t(v.getBitangent());
      FloatVector4  b(v.getTangent());
      FloatVector4  n(v.getNormal());
      ts.vertexTransform.transformXYZ(v.x, v.y, v.z);
      t = ts.vertexTransform.rotateXYZ(t);
      b = ts.vertexTransform.rotateXYZ(b);
      n = ts.vertexTransform.rotateXYZ(n);
      printToString(tmpBuf,
                    "  %5d: XYZ: %f, %f, %f  UV: %f, %f  VC: 0x%08X\n",
                    int(i), v.x, v.y, v.z, v.getU(), v.getV(),
                    (unsigned int) v.vertexColor);
      printToString(tmpBuf,
                    "         T: %6.3f, %6.3f, %6.3f  B: %6.3f, %6.3f, %6.3f  "
                    "N: %6.3f, %6.3f, %6.3f\n",
                    t[0], t[1], t[2], b[0], b[1], b[2], n[0], n[1], n[2]);
    }
    printToString(tmpBuf, "Triangle data:\n");
    for (size_t i = 0; i < ts.triangleCnt; i++)
    {
      printToString(tmpBuf, "  %6d: %5d, %5d, %5d\n",
                    int(i), int(ts.triangleData[i].v0),
                    int(ts.triangleData[i].v1), int(ts.triangleData[i].v2));
    }
  }
  if (!tmpBuf.empty())
  {
    if (mouseButton == 2 || mouseButton == 3)
      display.clearTextBuffer();
    display.printString(tmpBuf.c_str());
    if (mouseButton == 3)
      printMaterialInfo(display, ts.m);
    (void) SDL_SetClipboardText(tmpBuf.c_str());
    return true;
  }
  return false;
}

static const char *keyboardUsageString =
    "  \033[4m\033[38;5;228m0\033[m "
    "to \033[4m\033[38;5;228m5\033[m                "
    "Set debug render mode.                                          \n"
    "  \033[4m\033[38;5;228m+\033[m, "
    "\033[4m\033[38;5;228m-\033[m                  "
    "Zoom in or out.                                                 \n"
    "  \033[4m\033[38;5;228mKeypad 0, 5\033[m           "
    "Set view from the bottom or top.                                \n"
    "  \033[4m\033[38;5;228mKeypad 1 to 9\033[m         "
    "Set isometric view from the SW to NE (default = NW).            \n"
    "  \033[4m\033[38;5;228mShift + Keypad 0 to 9\033[m "
    "Set side view, or top/bottom view rotated by 45 degrees.        \n"
    "  \033[4m\033[38;5;228mF1\033[m "
    "to \033[4m\033[38;5;228mF8\033[m              "
    "Select default cube map.                                        \n"
    "  \033[4m\033[38;5;228mG\033[m                     "
    "Toggle rendering editor markers and geometry flagged as hidden. \n"
    "  \033[4m\033[38;5;228mA\033[m, "
    "\033[4m\033[38;5;228mD\033[m                  "
    "Rotate model around the Z axis.                                 \n"
    "  \033[4m\033[38;5;228mS\033[m, "
    "\033[4m\033[38;5;228mW\033[m                  "
    "Rotate model around the X axis.                                 \n"
    "  \033[4m\033[38;5;228mQ\033[m, "
    "\033[4m\033[38;5;228mE\033[m                  "
    "Rotate model around the Y axis.                                 \n"
    "  \033[4m\033[38;5;228mK\033[m, "
    "\033[4m\033[38;5;228mL\033[m                  "
    "Decrease or increase overall brightness.                        \n"
    "  \033[4m\033[38;5;228mU\033[m, "
    "\033[4m\033[38;5;228m7\033[m                  "
    "Decrease or increase light source red level.                    \n"
    "  \033[4m\033[38;5;228mI\033[m, "
    "\033[4m\033[38;5;228m8\033[m                  "
    "Decrease or increase light source green level.                  \n"
    "  \033[4m\033[38;5;228mO\033[m, "
    "\033[4m\033[38;5;228m9\033[m                  "
    "Decrease or increase light source blue level.                   \n"
    "  \033[4m\033[38;5;228mLeft\033[m, "
    "\033[4m\033[38;5;228mRight\033[m           "
    "Rotate light vector around the Z axis.                          \n"
    "  \033[4m\033[38;5;228mUp\033[m, "
    "\033[4m\033[38;5;228mDown\033[m              "
    "Rotate light vector around the Y axis.                          \n"
    "  \033[4m\033[38;5;228mHome\033[m                  "
    "Reset rotations.                                                \n"
    "  \033[4m\033[38;5;228mInsert\033[m, "
    "\033[4m\033[38;5;228mDelete\033[m        "
    "Zoom reflected environment in or out.                           \n"
    "  \033[4m\033[38;5;228mCaps Lock\033[m             "
    "Toggle fine adjustment of view and lighting parameters.         \n"
    "  \033[4m\033[38;5;228mPage Up\033[m               "
    "Enable downsampling (slow).                                     \n"
    "  \033[4m\033[38;5;228mPage Down\033[m             "
    "Disable downsampling.                                           \n"
    "  \033[4m\033[38;5;228mB\033[m                     "
    "Cycle background type (black/checkerboard/gradient).            \n"
    "  \033[4m\033[38;5;228mSpace\033[m, "
    "\033[4m\033[38;5;228mBackspace\033[m      "
    "Load next or previous file matching the pattern.                \n"
    "  \033[4m\033[38;5;228mF9\033[m                    "
    "Select file from list.                                          \n"
    "  \033[4m\033[38;5;228mF12\033[m "
    "or \033[4m\033[38;5;228mPrint Screen\033[m   "
    "Save screenshot.                                                \n"
    "  \033[4m\033[38;5;228mF11\033[m                   "
    "Save high quality screenshot (double resolution and downsample).\n"
    "  \033[4m\033[38;5;228mP\033[m                     "
    "Print current settings and file name, and copy the full path.   \n"
    "  \033[4m\033[38;5;228mV\033[m                     "
    "View detailed model information.                                \n"
    "  \033[4m\033[38;5;228mMouse buttons\033[m         "
    "In debug mode 1 only: print the TriShape block and material path\n"
    "                        "
    "of the selected shape based on the color of the pixel, and also \n"
    "                        "
    "copy it to the clipboard.                                       \n"
    "  \033[4m\033[38;5;228mH\033[m                     "
    "Show help screen.                                               \n"
    "  \033[4m\033[38;5;228mC\033[m                     "
    "Clear messages.                                                 \n"
    "  \033[4m\033[38;5;228mEsc\033[m                   "
    "Quit viewer.                                                    \n";
#endif

static const char *downsampModeNames[4] =
{
  "disabled", "enabled (4x SSAA)", "enabled (16x SSAA)", ""
};

bool NIF_View::viewModels(SDLDisplay& display,
                          const std::vector< std::string >& nifFileNames)
{
#ifdef HAVE_SDL2
  if (nifFileNames.size() < 1)
    return true;
  std::vector< SDLDisplay::SDLEvent > eventBuf;
  std::string messageBuf;
  unsigned char quitFlag = 0;   // 1 = Esc key pressed, 2 = window closed
  unsigned char backgroundType = 1;
  try
  {
    int     imageWidth = display.getWidth();
    int     imageHeight = display.getHeight();
    int     viewRotation = -1;  // < 0: do not change
    float   lightRotationX = 0.0f;
    int     fileNum = 0;
    int     d = 4;              // scale of adjusting parameters
    if (nifFileNames.size() > 10)
    {
      int     n = display.browseFile(nifFileNames, "Select file", fileNum,
                                     0x0B080F04FFFFULL);
      if (n >= 0 && size_t(n) < nifFileNames.size())
        fileNum = n;
      else if (n < -1)
        quitFlag = 2;
    }

    size_t  imageDataSize = size_t(imageWidth) * size_t(imageHeight);
    std::vector< float >  outBufZ(imageDataSize);
    while (!quitFlag)
    {
      messageBuf += nifFileNames[fileNum];
      messageBuf += '\n';
      loadModel(nifFileNames[fileNum]);

      bool    nextFileFlag = false;
      // 1: screenshot, 2: high quality screenshot, 4: view info, 8: browse file
      // 16: view text buffer
      unsigned char eventFlags = 0;
      unsigned char redrawFlags = 3;    // bit 0: blit only, bit 1: render
      while (!(nextFileFlag || quitFlag))
      {
        if (!messageBuf.empty())
        {
          display.printString(messageBuf.c_str());
          messageBuf.clear();
        }
        if (redrawFlags & 2)
        {
          if (viewRotation >= 0)
          {
            viewRotationX = viewRotations[viewRotation * 3];
            viewRotationY = viewRotations[viewRotation * 3 + 1];
            viewRotationZ = viewRotations[viewRotation * 3 + 2];
            display.printString(viewRotationMessages[viewRotation]);
            viewRotation = -1;
          }
          display.clearSurface((backgroundType == 0 ?
                                0U : (backgroundType == 1 ?
                                      0x02666333U : 01333111222U)), true);
          memsetFloat(outBufZ.data(), 16777216.0f, imageDataSize);
          std::uint32_t *outBufRGBA = display.lockDrawSurface();
          renderModel(outBufRGBA, outBufZ.data(), imageWidth, imageHeight);
          display.unlockDrawSurface();
          if (eventFlags)
          {
            if (eventFlags & 2)
              saveScreenshot(display, nifFileNames[fileNum], this);
            else if (eventFlags & 1)
              saveScreenshot(display, nifFileNames[fileNum]);
            if (eventFlags & 8)
            {
              int     n = display.browseFile(
                              nifFileNames, "Select file", fileNum,
                              0x0B080F04FFFFULL);
              if (n >= 0 && size_t(n) < nifFileNames.size() && n != fileNum)
              {
                fileNum = n;
                nextFileFlag = true;
              }
              else if (n < -1)
              {
                quitFlag = 2;
              }
            }
            else if ((eventFlags & 4) && nifFile)
            {
              const std::string&  fileName = nifFileNames[fileNum];
              if (fileName.starts_with("materials/") &&
                  (fileName.ends_with(".bgsm") || fileName.ends_with(".bgem")))
              {
                if (!viewMaterialInfo(display, ba2File, fileName))
                  quitFlag = 2;
              }
              else if (!viewModelInfo(display, *nifFile))
              {
                quitFlag = 2;
              }
              display.clearTextBuffer();
            }
            else if (eventFlags & 16)
            {
              if (!display.viewTextBuffer())
                quitFlag = 2;
              display.clearTextBuffer();
            }
            eventFlags = 0;
          }
          display.drawText(0, -1, display.getTextRows(), 0.75f, 1.0f);
          redrawFlags = 1;
        }
        display.blitSurface();
        redrawFlags = 0;

        while (!(redrawFlags || nextFileFlag || quitFlag))
        {
          display.pollEvents(eventBuf, -1000, false, true);
          for (size_t i = 0; i < eventBuf.size(); i++)
          {
            int     t = eventBuf[i].type();
            int     d1 = eventBuf[i].data1();
            if (t == SDLDisplay::SDLEventWindow)
            {
              if (d1 == 0)
                quitFlag = 2;
              else if (d1 == 1)
                redrawFlags = redrawFlags | 1;
              continue;
            }
            if (!(t == SDLDisplay::SDLEventKeyRepeat ||
                  t == SDLDisplay::SDLEventKeyDown))
            {
              if (t == SDLDisplay::SDLEventMButtonDown)
              {
                int     x = std::min(std::max(d1, 0), imageWidth - 1);
                int     y = eventBuf[i].data2();
                y = std::min(std::max(y, 0), imageHeight - 1);
                int     mouseButton = eventBuf[i].data3();
                if (debugMode == 1 && eventBuf[i].data4() >= 1 &&
                    printGeometryBlockInfo(display, x, y, mouseButton,
                                           nifFile, meshData))
                {
                  if (mouseButton == 2 || mouseButton == 3)
                    eventFlags = eventFlags | 16;
                  redrawFlags = redrawFlags | 2;
                }
              }
              continue;
            }
            redrawFlags = 2;
            switch (d1)
            {
              case '0':
              case '1':
              case '2':
              case '3':
              case '4':
              case '5':
                debugMode = int(d1 - '0');
                messageBuf += "Debug mode set to ";
                messageBuf += char(d1);
                messageBuf += '\n';
                break;
              case '-':
              case SDLDisplay::SDLKeySymKPMinus:
                updateValueLogScale(viewScale, -d, 0.0625f, 16.0f, messageBuf,
                                    "View scale");
                break;
              case '=':
              case SDLDisplay::SDLKeySymKPPlus:
                updateValueLogScale(viewScale, d, 0.0625f, 16.0f, messageBuf,
                                    "View scale");
                break;
              case SDLDisplay::SDLKeySymKP1:
              case SDLDisplay::SDLKeySymKP2:
              case SDLDisplay::SDLKeySymKP3:
              case SDLDisplay::SDLKeySymKP4:
              case SDLDisplay::SDLKeySymKP5:
              case SDLDisplay::SDLKeySymKP6:
              case SDLDisplay::SDLKeySymKP7:
              case SDLDisplay::SDLKeySymKP8:
              case SDLDisplay::SDLKeySymKP9:
              case SDLDisplay::SDLKeySymKPIns:
                viewRotation = getViewRotation(d1, eventBuf[i].data2());
                break;
              case SDLDisplay::SDLKeySymF1:
              case SDLDisplay::SDLKeySymF2:
              case SDLDisplay::SDLKeySymF3:
              case SDLDisplay::SDLKeySymF4:
              case SDLDisplay::SDLKeySymF5:
              case SDLDisplay::SDLKeySymF6:
              case SDLDisplay::SDLKeySymF7:
              case SDLDisplay::SDLKeySymF8:
                setDefaultTextures(d1 - SDLDisplay::SDLKeySymF1);
                messageBuf += "Default environment map: ";
                messageBuf += defaultEnvMap;
                messageBuf += '\n';
                break;
              case 'g':
                enableHidden = !enableHidden;
                printToString(messageBuf, "Markers and hidden geometry %s\n",
                              (!enableHidden ? "disabled" : "enabled"));
                break;
              case SDLDisplay::SDLKeySymF9:
                eventFlags = eventFlags | 8;    // browse file list
                break;
              case 'a':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, 0, d, messageBuf, "Model rotation");
                break;
              case 'd':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, 0, -d, messageBuf, "Model rotation");
                break;
              case 's':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               d, 0, 0, messageBuf, "Model rotation");
                break;
              case 'w':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               -d, 0, 0, messageBuf, "Model rotation");
                break;
              case 'q':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, -d, 0, messageBuf, "Model rotation");
                break;
              case 'e':
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, d, 0, messageBuf, "Model rotation");
                break;
              case 'k':
                updateValueLogScale(rgbScale, -d, 0.0625f, 16.0f, messageBuf,
                                    "Brightness (linear color space)");
                break;
              case 'l':
                updateValueLogScale(rgbScale, d, 0.0625f, 16.0f, messageBuf,
                                    "Brightness (linear color space)");
                break;
              case SDLDisplay::SDLKeySymLeft:
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, 0, d, messageBuf, "Light rotation");
                break;
              case SDLDisplay::SDLKeySymRight:
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, 0, -d, messageBuf, "Light rotation");
                break;
              case SDLDisplay::SDLKeySymDown:
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, d, 0, messageBuf, "Light rotation");
                break;
              case SDLDisplay::SDLKeySymUp:
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, -d, 0, messageBuf, "Light rotation");
                break;
              case SDLDisplay::SDLKeySymHome:
                messageBuf = "Model and light rotations reset to defaults";
                modelRotationX = 0.0f;
                modelRotationY = 0.0f;
                modelRotationZ = 0.0f;
                lightRotationY = 56.25f;
                lightRotationZ = -135.0f;
                break;
              case '7':
                updateLightColor(lightColor, d, 0, 0, messageBuf);
                break;
              case 'u':
                updateLightColor(lightColor, -d, 0, 0, messageBuf);
                break;
              case '8':
                updateLightColor(lightColor, 0, d, 0, messageBuf);
                break;
              case 'i':
                updateLightColor(lightColor, 0, -d, 0, messageBuf);
                break;
              case '9':
                updateLightColor(lightColor, 0, 0, d, messageBuf);
                break;
              case 'o':
                updateLightColor(lightColor, 0, 0, -d, messageBuf);
                break;
              case SDLDisplay::SDLKeySymInsert:
                updateValueLogScale(reflZScale, d, 0.25f, 8.0f, messageBuf,
                                    "Reflection f scale");
                break;
              case SDLDisplay::SDLKeySymDelete:
                updateValueLogScale(reflZScale, -d, 0.25f, 8.0f, messageBuf,
                                    "Reflection f scale");
                break;
              case SDLDisplay::SDLKeySymCapsLock:
                d = (d == 1 ? 4 : 1);
                if (d == 1)
                  messageBuf += "Step size: 2.8125\302\260, exp2(1/16)\n";
                else
                  messageBuf += "Step size: 11.25\302\260, exp2(1/4)\n";
                break;
              case SDLDisplay::SDLKeySymPageUp:
              case SDLDisplay::SDLKeySymPageDown:
                {
                  int     prvLevel = display.getDownsampleLevel();
                  display.setDownsampleLevel(
                      prvLevel + (d1 == SDLDisplay::SDLKeySymPageUp ? 1 : -1));
                  if (display.getDownsampleLevel() == (unsigned char) prvLevel)
                  {
                    redrawFlags = 0;
                    continue;
                  }
                }
                imageWidth = display.getWidth();
                imageHeight = display.getHeight();
                imageDataSize = size_t(imageWidth) * size_t(imageHeight);
                outBufZ.resize(imageDataSize);
                printToString(messageBuf, "Downsampling %s\n",
                              downsampModeNames[display.getDownsampleLevel()]);
                break;
              case 'b':
                backgroundType = (unsigned char) ((backgroundType + 1) % 3);
                break;
              case SDLDisplay::SDLKeySymBackspace:
                fileNum = (fileNum > 0 ? fileNum : int(nifFileNames.size()));
                fileNum--;
                nextFileFlag = true;
                break;
              case ' ':
                fileNum++;
                fileNum = (size_t(fileNum) < nifFileNames.size() ? fileNum : 0);
                nextFileFlag = true;
                break;
              case SDLDisplay::SDLKeySymF11:
                eventFlags = eventFlags | 2;    // high quality screenshot
                break;
              case SDLDisplay::SDLKeySymF12:
              case SDLDisplay::SDLKeySymPrintScr:
                eventFlags = eventFlags | 1;    // screenshot
                break;
              case 'p':
                display.clearTextBuffer();
                updateRotation(modelRotationX, modelRotationY, modelRotationZ,
                               0, 0, 0, messageBuf, "Model rotation");
                display.printString(messageBuf.c_str());
                updateRotation(lightRotationX, lightRotationY, lightRotationZ,
                               0, 0, 0, messageBuf, "Light rotation");
                display.printString(messageBuf.c_str());
                updateValueLogScale(rgbScale, 0, 0.25f, 4.0f, messageBuf,
                                    "Brightness (linear color space)");
                display.printString(messageBuf.c_str());
                updateLightColor(lightColor, 0, 0, 0, messageBuf);
                display.printString(messageBuf.c_str());
                printViewScale(display, viewScale, viewTransform);
                updateValueLogScale(reflZScale, 0, 0.5f, 4.0f, messageBuf,
                                    "Reflection f scale");
                if (d == 1)
                  messageBuf += "Step size: 2.8125\302\260, exp2(1/16)\n";
                else
                  messageBuf += "Step size: 11.25\302\260, exp2(1/4)\n";
                printToString(messageBuf, "Downsampling %s\n",
                              downsampModeNames[display.getDownsampleLevel()]);
                printToString(messageBuf, "Default environment map: %s\n",
                              defaultEnvMap.c_str());
                printToString(messageBuf, "Markers and hidden geometry %s\n",
                              (!enableHidden ? "disabled" : "enabled"));
                messageBuf += "File name:\n  \033[44m\033[37m\033[1m";
                messageBuf += nifFileNames[fileNum];
                messageBuf += "\033[m  \n";
                (void) SDL_SetClipboardText(nifFileNames[fileNum].c_str());
                continue;
              case 'v':
                eventFlags = eventFlags | 4;    // view model info
                break;
              case 'h':
                messageBuf = keyboardUsageString;
                break;
              case 'c':
                break;
              case SDLDisplay::SDLKeySymEscape:
                quitFlag = 1;
                break;
              default:
                redrawFlags = 0;
                continue;
            }
            display.clearTextBuffer();
          }
        }
      }
    }
  }
  catch (std::exception& e)
  {
    display.unlockScreenSurface();
    messageBuf += "\033[41m\033[33m\033[1m    Error: ";
    messageBuf += e.what();
    messageBuf += "    ";
    display.printString(messageBuf.c_str());
    display.drawText(0, -1, display.getTextRows(), 1.0f, 1.0f);
    display.blitSurface();
    do
    {
      display.pollEvents(eventBuf);
      for (size_t i = 0; i < eventBuf.size(); i++)
      {
        if ((eventBuf[i].type() == SDLDisplay::SDLEventWindow &&
             eventBuf[i].data1() == 0) ||
            eventBuf[i].type() == SDLDisplay::SDLEventKeyDown)
        {
          quitFlag = (eventBuf[i].type() == SDLDisplay::SDLEventWindow ? 2 : 1);
          break;
        }
        else if (eventBuf[i].type() == SDLDisplay::SDLEventWindow &&
                 eventBuf[i].data1() == 1)
        {
          display.blitSurface();
        }
      }
    }
    while (!quitFlag);
  }
  return (quitFlag < 2);
#else
  (void) display;
  (void) nifFileNames;
  return false;
#endif
}


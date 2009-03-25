/* $Id$
-----------------------------------------------------------------------------

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place - Suite 330, Boston, MA 02111-1307, USA, or go to
http://www.gnu.org/copyleft/lesser.txt.

Contributor(s): Dalai Felinto

This code is originally inspired on some of the ideas and codes from Paul Bourke.
Developed as part of a Research and Development project for SAT - La Soci�t� des arts technologiques.
-----------------------------------------------------------------------------
*/

#include <PyObjectPlus.h>
#include <structmember.h>
#include <float.h>
#include <math.h>


#include <BIF_gl.h>

#include "KX_PythonInit.h"
#include "DNA_scene_types.h"
#include "RAS_CameraData.h"
#include "BLI_arithb.h"

#include "KX_Dome.h"

#include "GL/glew.h"
#include "GPU_extensions.h"
#include "GL/glu.h" //XXX temporary, I don't think Blender can use glu.h in its files!!!

// constructor
KX_Dome::KX_Dome (
	RAS_ICanvas* canvas,
    /// rasterizer
    RAS_IRasterizer* rasterizer,
    /// render tools
    RAS_IRenderTools* rendertools,
    /// engine
    KX_KetsjiEngine* engine,
	
	float size,		//size for adjustments
	short res,		//resolution of the mesh
	short mode,		//mode - fisheye, truncated, warped, panoramic, ...
	short angle,
	float resbuf	//size adjustment of the buffer

):
	m_canvas(canvas),
	m_rasterizer(rasterizer),
	m_rendertools(rendertools),
	m_engine(engine),
	m_clip(100.f),
	m_drawingmode(engine->GetDrawType()),
	m_size(size),
	m_resolution(res),
	m_mode(mode),
	m_angle(angle),
	m_resbuffer(resbuf),
	canvaswidth(-1), canvasheight(-1)
{
	if (mode > DOME_NUM_MODES)
		m_mode = DOME_FISHEYE;
	
	//setting the viewport size
	GLuint	viewport[4]={0};
	glGetIntegerv(GL_VIEWPORT,(GLint *)viewport);

	SetViewPort(viewport);

	//4 == 180�; 5 == 250�; 6 == 360�
	m_numfaces = 5;
//	if (m_angle > 250)
//		m_angle = 250;

	switch(m_mode){
		case DOME_FISHEYE:
			if (m_angle <= 180){
				cubetop.resize(1);
				cubebottom.resize(1);
				cubeleft.resize(2);
				cuberight.resize(2);

				CreateMeshDome180(m_resolution);
				m_numfaces = 4;
			}else if (m_angle > 180 && m_angle <= 250){
				cubetop.resize(2);
				cubebottom.resize(2);
				cubeleft.resize(2);
				cubefront.resize(2);
				cuberight.resize(2);

				CreateMeshDome250(m_resolution);
				m_numfaces = 5;
			}else{
				cubetop.resize(2);
				cubebottom.resize(2);
				cubeleft.resize(2);
				cubefront.resize(2);
				cuberight.resize(2);
				cubeback.resize(2);

				CreateMeshDome250(m_resolution);
				m_numfaces = 6;
			}
			break;
		case DOME_TRUNCATED:
			cubetop.resize(1);
			cubebottom.resize(1);
			cubeleft.resize(2);
			cuberight.resize(2);

			m_angle = 180;
			CreateMeshDome180(m_resolution);
			m_numfaces = 4;
			break;
		case DOME_PANORAM_SPH:
			cubeleft.resize(2);
			cubeleftback.resize(2);
			cuberight.resize(2);
			cuberightback.resize(2);

			m_angle = 360;
			CreateMeshPanorama();
			m_numfaces = 4;
			break;
		case DOME_OFFSET:
			//the same as DOME_FISHEYE > 250�
			cubetop.resize(2);
			cubebottom.resize(2);
			cubeleft.resize(2);
			cubefront.resize(2);
			cuberight.resize(2);
			cubeback.resize(2);

//		m_offset = 0.99;
			m_angle = 360;
			CreateMeshDome250(m_resolution);
			m_numfaces = 6;
			break;
		default: // temporary
			m_angle = 360;
			m_numfaces = 6;
			break;
	}

	CalculateCameraOrientation();

	CreateGLImages();
	//openGL check 
	if(GLEW_VERSION_1_1){
		dlistSupported = true;
		CreateDL();
	}
}

// destructor
KX_Dome::~KX_Dome (void)
{
	ClearGLImages();

	if(dlistSupported)
		glDeleteLists(dlistId, (GLsizei) m_numfaces);
}

void KX_Dome::CreateGLImages(void){
	glGenTextures(m_numfaces, (GLuint*)&domefacesId);

	for (int j=0;j<m_numfaces;j++){
		glBindTexture(GL_TEXTURE_2D, domefacesId[j]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, m_imagesize, m_imagesize, 0, GL_RGBA,
				GL_UNSIGNED_BYTE, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
}

void KX_Dome::ClearGLImages(void)
{
	glDeleteTextures(m_numfaces, (GLuint*)&domefacesId);
/*
	for (int i=0;i<m_numfaces;i++)
		if(glIsTexture(domefacesId[i]))
			glDeleteTextures(1, (GLuint*)&domefacesId[i]);
*/
}

void KX_Dome::CalculateImageSize(void)
{
/*
- determine the minimum buffer size
- reduce the buffer for better performace
- create a power of 2 texture bigger than the buffer
*/

	canvaswidth = m_canvas->GetWidth();
	canvasheight = m_canvas->GetHeight();

	m_buffersize = (canvaswidth > canvasheight?canvasheight:canvaswidth);
	m_buffersize *= m_resbuffer; //reduce buffer size for better performance

	int i = 0;
	while ((1 << i) <= m_buffersize)
		i++;
	m_imagesize = (1 << i);
}

void KX_Dome::CreateDL(){
	int i,j;

	dlistId = glGenLists((GLsizei) m_numfaces);
	if (dlistId != 0) {
		if(m_mode == DOME_FISHEYE || m_mode == DOME_TRUNCATED || m_mode == DOME_WARPED || m_mode == DOME_OFFSET){
			glNewList(dlistId, GL_COMPILE);
				GLDrawTriangles(cubetop, nfacestop);
			glEndList();

			glNewList(dlistId+1, GL_COMPILE);
				GLDrawTriangles(cubebottom, nfacesbottom);
			glEndList();

			glNewList(dlistId+2, GL_COMPILE);
				GLDrawTriangles(cubeleft, nfacesleft);
			glEndList();

			glNewList(dlistId+3, GL_COMPILE);
				GLDrawTriangles(cuberight, nfacesright);
			glEndList();

			if (m_angle > 180){
				glNewList(dlistId+4, GL_COMPILE);
					GLDrawTriangles(cubefront, nfacesfront);
				glEndList();
			}
		}
		else if (m_mode == DOME_PANORAM_SPH){
			glNewList(dlistId, GL_COMPILE);
				GLDrawTriangles(cubeleft, nfacesleft);
			glEndList();

			glNewList(dlistId+1, GL_COMPILE);
				GLDrawTriangles(cuberight, nfacesright);
			glEndList();

			glNewList(dlistId+3, GL_COMPILE);
				GLDrawTriangles(cubeleftback, nfacesleftback);
			glEndList();

			glNewList(dlistId+2, GL_COMPILE);
				GLDrawTriangles(cuberightback, nfacesrightback);
			glEndList();
		}

		//clearing the vectors 
		cubetop.clear();
		cubebottom.clear();
		cuberight.clear();
		cubeleft.clear();
		cubefront.clear();
		cubeback.clear();
		cubeleftback.clear();
		cuberightback.clear();

	} else // genList failed
		dlistSupported = false;
}

void KX_Dome::GLDrawTriangles(vector <DomeFace>& face, int nfaces)
{
	int i,j;
	glBegin(GL_TRIANGLES);
		for (i=0;i<nfaces;i++) {
			for (j=0;j<3;j++) {
				glTexCoord2f(face[i].u[j],face[i].v[j]);
				glVertex3f((GLfloat)face[i].verts[j][0],(GLfloat)face[i].verts[j][1],(GLfloat)face[i].verts[j][2]);
			}
		}
	glEnd();
}
void KX_Dome::CreateMeshDome180(int cubeRes)
{
/*
1)-  Define the faces of half of a cube 
 - each face is made out of 2 triangles
2) Subdivide the faces
 - more resolution == more curved lines
3) Spherize the cube
 - normalize the verts
4) Flatten onto xz plane
 - transform it onto an equidistant spherical projection techniques to transform the sphere onto a dome image
*/
	int i,j;
	float sqrt_2 = sqrt(2.0);

	float uv_ratio;
	uv_ratio = (float)m_buffersize / m_imagesize;

	//creating faces for the env mapcube 180� Dome
	// Top Face - just a triangle
	cubetop[0].verts[0][0] = -sqrt_2 / 2.0;
	cubetop[0].verts[0][1] = 0.0;
	cubetop[0].verts[0][2] = 0.5;
	cubetop[0].u[0] = 0.0;
	cubetop[0].v[0] = uv_ratio;

	cubetop[0].verts[1][0] = 0.0;
	cubetop[0].verts[1][1] = sqrt_2 / 2.0;
	cubetop[0].verts[1][2] = 0.5;
	cubetop[0].u[1] = 0.0;
	cubetop[0].v[1] = 0.0;

	cubetop[0].verts[2][0] = sqrt_2 / 2.0;
	cubetop[0].verts[2][1] = 0.0;
	cubetop[0].verts[2][2] = 0.5;
	cubetop[0].u[2] = uv_ratio;
	cubetop[0].v[2] = 0.0;

	nfacestop = 1;

	/* Bottom face - just a triangle */
	cubebottom[0].verts[0][0] = -sqrt_2 / 2.0;
	cubebottom[0].verts[0][1] = 0.0;
	cubebottom[0].verts[0][2] = -0.5;
	cubebottom[0].u[0] = uv_ratio;
	cubebottom[0].v[0] = 0.0;

	cubebottom[0].verts[1][0] = sqrt_2 / 2.0;
	cubebottom[0].verts[1][1] = 0;
	cubebottom[0].verts[1][2] = -0.5;
	cubebottom[0].u[1] = 0.0;
	cubebottom[0].v[1] = uv_ratio;

	cubebottom[0].verts[2][0] = 0.0;
	cubebottom[0].verts[2][1] = sqrt_2 / 2.0;
	cubebottom[0].verts[2][2] = -0.5;
	cubebottom[0].u[2] = 0.0;
	cubebottom[0].v[2] = 0.0;

	nfacesbottom = 1;	
	
	/* Left face - two triangles */
	
	cubeleft[0].verts[0][0] = -sqrt_2 / 2.0;
	cubeleft[0].verts[0][1] = .0;
	cubeleft[0].verts[0][2] = -0.5;
	cubeleft[0].u[0] = 0.0;
	cubeleft[0].v[0] = 0.0;

	cubeleft[0].verts[1][0] = 0.0;
	cubeleft[0].verts[1][1] = sqrt_2 / 2.0;
	cubeleft[0].verts[1][2] = -0.5;
	cubeleft[0].u[1] = uv_ratio;
	cubeleft[0].v[1] = 0.0;

	cubeleft[0].verts[2][0] = -sqrt_2 / 2.0;
	cubeleft[0].verts[2][1] = 0.0;
	cubeleft[0].verts[2][2] = 0.5;
	cubeleft[0].u[2] = 0.0;
	cubeleft[0].v[2] = uv_ratio;

	cubeleft[1].verts[0][0] = -sqrt_2 / 2.0;
	cubeleft[1].verts[0][1] = 0.0;
	cubeleft[1].verts[0][2] = 0.5;
	cubeleft[1].u[0] = 0.0;
	cubeleft[1].v[0] = uv_ratio;

	cubeleft[1].verts[1][0] = 0.0;
	cubeleft[1].verts[1][1] = sqrt_2 / 2.0;
	cubeleft[1].verts[1][2] = -0.5;
	cubeleft[1].u[1] = uv_ratio;
	cubeleft[1].v[1] = 0.0;

	cubeleft[1].verts[2][0] = 0.0;
	cubeleft[1].verts[2][1] = sqrt_2 / 2.0;
	cubeleft[1].verts[2][2] = 0.5;
	cubeleft[1].u[2] = uv_ratio;
	cubeleft[1].v[2] = uv_ratio;

	nfacesleft = 2;
	
	/* Right face - two triangles */
	cuberight[0].verts[0][0] = 0.0;
	cuberight[0].verts[0][1] = sqrt_2 / 2.0;
	cuberight[0].verts[0][2] = -0.5;
	cuberight[0].u[0] = 0.0;
	cuberight[0].v[0] = 0.0;

	cuberight[0].verts[1][0] = sqrt_2 / 2.0;
	cuberight[0].verts[1][1] = 0.0;
	cuberight[0].verts[1][2] = -0.5;
	cuberight[0].u[1] = uv_ratio;
	cuberight[0].v[1] = 0.0;

	cuberight[0].verts[2][0] = sqrt_2 / 2.0;
	cuberight[0].verts[2][1] = 0.0;
	cuberight[0].verts[2][2] = 0.5;
	cuberight[0].u[2] = uv_ratio;
	cuberight[0].v[2] = uv_ratio;

	cuberight[1].verts[0][0] = 0.0;
	cuberight[1].verts[0][1] = sqrt_2 / 2.0;
	cuberight[1].verts[0][2] = -0.5;
	cuberight[1].u[0] = 0.0;
	cuberight[1].v[0] = 0.0;

	cuberight[1].verts[1][0] = sqrt_2 / 2.0;
	cuberight[1].verts[1][1] = 0.0;
	cuberight[1].verts[1][2] = 0.5;
	cuberight[1].u[1] = uv_ratio;
	cuberight[1].v[1] = uv_ratio;

	cuberight[1].verts[2][0] = 0.0;
	cuberight[1].verts[2][1] = sqrt_2 / 2.0;
	cuberight[1].verts[2][2] = 0.5;
	cuberight[1].u[2] = 0.0;
	cuberight[1].v[2] = uv_ratio;

	nfacesright = 2;
	
	//Refine a triangular mesh by bisecting each edge forms 3 new triangles for each existing triangle on each iteration
	//Could be made more efficient for drawing if the triangles were ordered in a fan. Not that important since we are using DisplayLists

	for(i=0;i<cubeRes;i++){
		cubetop.resize(4*nfacestop);
		SplitFace(cubetop,&nfacestop);
		cubebottom.resize(4*nfacesbottom);
		SplitFace(cubebottom,&nfacesbottom);	
		cubeleft.resize(4*nfacesleft);
		SplitFace(cubeleft,&nfacesleft);
		cuberight.resize(4*nfacesright);
		SplitFace(cuberight,&nfacesright);
	}		

	// Turn into a hemisphere
	for(j=0;j<3;j++){
		for(i=0;i<nfacestop;i++)
			cubetop[i].verts[j].normalize();
		for(i=0;i<nfacesbottom;i++)
			cubebottom[i].verts[j].normalize();
		for(i=0;i<nfacesleft;i++)
			cubeleft[i].verts[j].normalize();
		for(i=0;i<nfacesright;i++)
			cuberight[i].verts[j].normalize();
	}
	
	//flatten onto xz plane
	for(i=0;i<nfacestop;i++)
		FlattenDome(cubetop[i].verts);
	for(i=0;i<nfacesbottom;i++)
		FlattenDome(cubebottom[i].verts);
	for(i=0;i<nfacesleft;i++)
		FlattenDome(cubeleft[i].verts);
	for(i=0;i<nfacesright;i++)
		FlattenDome(cuberight[i].verts);

}

void KX_Dome::CreateMeshDome250(int resolution)
{
/*
1)-  Define the faces of a cube without the back face
 - each face is made out of 2 triangles
2) Subdivide the faces
 - more resolution == more curved lines
3) Spherize the cube
 - normalize the verts
4) Flatten onto xz plane
 - transform it onto an equidistant spherical projection techniques to transform the sphere onto a dome image
*/

	int i,j;
	float uv_height, uv_base;
	float uv_ratio;
	float verts_height;
	float rad_ang = m_angle * MT_PI / 180.0;

	verts_height = tan((rad_ang/2) - (MT_PI/2));//for 180 - M_PI/2 for 270 - 
	verts_height = 1.0;

	uv_ratio = (float)m_buffersize / m_imagesize;
	uv_height = (float)m_buffersize / m_imagesize;
//	uv_height = (verts_height/2) + 0.5;
//	uv_base = 1.0 - uv_height;

//	uv_height = 1.0;
	uv_base = 0.0;
	verts_height = 1.0;
	
	//creating faces for the env mapcube 180� Dome
	// Front Face - 2 triangles
	cubefront[0].verts[0][0] =-1.0;
	cubefront[0].verts[0][1] = 1.0;
	cubefront[0].verts[0][2] =-1.0;
	cubefront[0].u[0] = 0.0;
	cubefront[0].v[0] = 0.0;

	cubefront[0].verts[1][0] = 1.0;
	cubefront[0].verts[1][1] = 1.0;
	cubefront[0].verts[1][2] = 1.0;	
	cubefront[0].u[1] = uv_ratio;
	cubefront[0].v[1] = uv_ratio;

	cubefront[0].verts[2][0] =-1.0;
	cubefront[0].verts[2][1] = 1.0;
	cubefront[0].verts[2][2] = 1.0;	
	cubefront[0].u[2] = 0.0;
	cubefront[0].v[2] = uv_ratio;

	//second triangle
	cubefront[1].verts[0][0] = 1.0;
	cubefront[1].verts[0][1] = 1.0;
	cubefront[1].verts[0][2] = 1.0;
	cubefront[1].u[0] = uv_ratio;
	cubefront[1].v[0] = uv_ratio;

	cubefront[1].verts[1][0] =-1.0;
	cubefront[1].verts[1][1] = 1.0;
	cubefront[1].verts[1][2] =-1.0;	
	cubefront[1].u[1] = 0.0;
	cubefront[1].v[1] = 0.0;

	cubefront[1].verts[2][0] = 1.0;
	cubefront[1].verts[2][1] = 1.0;
	cubefront[1].verts[2][2] =-1.0;	
	cubefront[1].u[2] = uv_ratio;
	cubefront[1].v[2] = 0.0;

	nfacesfront = 2;

	// Left Face - 2 triangles
	cubeleft[0].verts[0][0] =-1.0;
	cubeleft[0].verts[0][1] = 1.0;
	cubeleft[0].verts[0][2] =-1.0;
	cubeleft[0].u[0] = uv_ratio;
	cubeleft[0].v[0] = 0.0;

	cubeleft[0].verts[1][0] =-1.0;
	cubeleft[0].verts[1][1] =-verts_height;
	cubeleft[0].verts[1][2] = 1.0;	
	cubeleft[0].u[1] = uv_base;
	cubeleft[0].v[1] = uv_ratio;

	cubeleft[0].verts[2][0] =-1.0;
	cubeleft[0].verts[2][1] =-verts_height;
	cubeleft[0].verts[2][2] =-1.0;	
	cubeleft[0].u[2] = uv_base;
	cubeleft[0].v[2] = 0.0;

	//second triangle
	cubeleft[1].verts[0][0] =-1.0;
	cubeleft[1].verts[0][1] =-verts_height;
	cubeleft[1].verts[0][2] = 1.0;
	cubeleft[1].u[0] = uv_base;
	cubeleft[1].v[0] = uv_ratio;

	cubeleft[1].verts[1][0] =-1.0;
	cubeleft[1].verts[1][1] = 1.0;
	cubeleft[1].verts[1][2] =-1.0;	
	cubeleft[1].u[1] = uv_ratio;
	cubeleft[1].v[1] = 0.0;

	cubeleft[1].verts[2][0] =-1.0;
	cubeleft[1].verts[2][1] = 1.0;
	cubeleft[1].verts[2][2] = 1.0;	
	cubeleft[1].u[2] = uv_ratio;
	cubeleft[1].v[2] = uv_ratio;

	nfacesleft = 2;

	// right Face - 2 triangles
	cuberight[0].verts[0][0] = 1.0;
	cuberight[0].verts[0][1] = 1.0;
	cuberight[0].verts[0][2] = 1.0;
	cuberight[0].u[0] = 0.0;
	cuberight[0].v[0] = uv_ratio;

	cuberight[0].verts[1][0] = 1.0;
	cuberight[0].verts[1][1] =-verts_height;
	cuberight[0].verts[1][2] =-1.0;	
	cuberight[0].u[1] = uv_height;
	cuberight[0].v[1] = 0.0;

	cuberight[0].verts[2][0] = 1.0;
	cuberight[0].verts[2][1] =-verts_height;
	cuberight[0].verts[2][2] = 1.0;	
	cuberight[0].u[2] = uv_height;
	cuberight[0].v[2] = uv_ratio;

	//second triangle
	cuberight[1].verts[0][0] = 1.0;
	cuberight[1].verts[0][1] =-verts_height;
	cuberight[1].verts[0][2] =-1.0;
	cuberight[1].u[0] = uv_height;
	cuberight[1].v[0] = 0.0;

	cuberight[1].verts[1][0] = 1.0;
	cuberight[1].verts[1][1] = 1.0;
	cuberight[1].verts[1][2] = 1.0;	
	cuberight[1].u[1] = 0.0;
	cuberight[1].v[1] = uv_ratio;

	cuberight[1].verts[2][0] = 1.0;
	cuberight[1].verts[2][1] = 1.0;
	cuberight[1].verts[2][2] =-1.0;	
	cuberight[1].u[2] = 0.0;
	cuberight[1].v[2] = 0.0;

	nfacesright = 2;

	// top Face - 2 triangles
	cubetop[0].verts[0][0] =-1.0;
	cubetop[0].verts[0][1] = 1.0;
	cubetop[0].verts[0][2] = 1.0;
	cubetop[0].u[0] = 0.0;
	cubetop[0].v[0] = 0.0;

	cubetop[0].verts[1][0] = 1.0;
	cubetop[0].verts[1][1] =-verts_height;
	cubetop[0].verts[1][2] = 1.0;	
	cubetop[0].u[1] = uv_ratio;
	cubetop[0].v[1] = uv_height;

	cubetop[0].verts[2][0] =-1.0;
	cubetop[0].verts[2][1] =-verts_height;
	cubetop[0].verts[2][2] = 1.0;	
	cubetop[0].u[2] = 0.0;
	cubetop[0].v[2] = uv_height;

	//second triangle
	cubetop[1].verts[0][0] = 1.0;
	cubetop[1].verts[0][1] =-verts_height;
	cubetop[1].verts[0][2] = 1.0;
	cubetop[1].u[0] = uv_ratio;
	cubetop[1].v[0] = uv_height;

	cubetop[1].verts[1][0] =-1.0;
	cubetop[1].verts[1][1] = 1.0;
	cubetop[1].verts[1][2] = 1.0;	
	cubetop[1].u[1] = 0.0;
	cubetop[1].v[1] = 0.0;

	cubetop[1].verts[2][0] = 1.0;
	cubetop[1].verts[2][1] = 1.0;
	cubetop[1].verts[2][2] = 1.0;	
	cubetop[1].u[2] = uv_ratio;
	cubetop[1].v[2] = 0.0;

	nfacestop = 2;

	// bottom Face - 2 triangles
	cubebottom[0].verts[0][0] =-1.0;
	cubebottom[0].verts[0][1] =-1.0;
	cubebottom[0].verts[0][2] =-1.0;
	cubebottom[0].u[0] = 0.0;
	cubebottom[0].v[0] = 0.0;

	cubebottom[0].verts[1][0] = 1.0;
	cubebottom[0].verts[1][1] = 1.0;
	cubebottom[0].verts[1][2] =-1.0;	
	cubebottom[0].u[1] = uv_ratio;
	cubebottom[0].v[1] = uv_height;

	cubebottom[0].verts[2][0] =-1.0;
	cubebottom[0].verts[2][1] = 1.0;
	cubebottom[0].verts[2][2] =-1.0;	
	cubebottom[0].u[2] = 0.0;
	cubebottom[0].v[2] = uv_height;

	//second triangle
	cubebottom[1].verts[0][0] = 1.0;
	cubebottom[1].verts[0][1] = 1.0;
	cubebottom[1].verts[0][2] =-1.0;
	cubebottom[1].u[0] = uv_ratio;
	cubebottom[1].v[0] = uv_height;

	cubebottom[1].verts[1][0] =-1.0;
	cubebottom[1].verts[1][1] =-1.0;
	cubebottom[1].verts[1][2] =-1.0;	
	cubebottom[1].u[1] = 0.0;
	cubebottom[1].v[1] = 0.0;

	cubebottom[1].verts[2][0] = 1.0;
	cubebottom[1].verts[2][1] =-1.0;
	cubebottom[1].verts[2][2] =-1.0;	
	cubebottom[1].u[2] = uv_ratio;
	cubebottom[1].v[2] = 0.0;

	nfacesbottom = 2;

	if(m_angle > 250){
	
		cubeback[0].verts[0][0] = 1.0;
		cubeback[0].verts[0][1] =-1.0;
		cubeback[0].verts[0][2] =-1.0;
		cubeback[0].u[0] = 0.0;
		cubeback[0].v[0] = 0.0;

		cubeback[0].verts[1][0] =-1.0;
		cubeback[0].verts[1][1] =-1.0;
		cubeback[0].verts[1][2] = 1.0;	
		cubeback[0].u[1] = uv_ratio;
		cubeback[0].v[1] = uv_ratio;

		cubeback[0].verts[2][0] = 1.0;
		cubeback[0].verts[2][1] =-1.0;
		cubeback[0].verts[2][2] = 1.0;	
		cubeback[0].u[2] = 0.0;
		cubeback[0].v[2] = uv_ratio;

		//second triangle
		cubeback[1].verts[0][0] =-1.0;
		cubeback[1].verts[0][1] =-1.0;
		cubeback[1].verts[0][2] = 1.0;
		cubeback[1].u[0] = uv_ratio;
		cubeback[1].v[0] = uv_ratio;

		cubeback[1].verts[1][0] = 1.0;
		cubeback[1].verts[1][1] =-1.0;
		cubeback[1].verts[1][2] =-1.0;	
		cubeback[1].u[1] = 0.0;
		cubeback[1].v[1] = 0.0;

		cubeback[1].verts[2][0] =-1.0;
		cubeback[1].verts[2][1] =-1.0;
		cubeback[1].verts[2][2] =-1.0;	
		cubeback[1].u[2] = uv_ratio;
		cubeback[1].v[2] = 0.0;

		nfacesback = 2;
	
	}
	//Refine a triangular mesh by bisecting each edge forms 3 new triangles for each existing triangle on each iteration
	//It could be made more efficient for drawing if the triangles were ordered in a strip!

	for(i=0;i<resolution;i++){
		cubefront.resize(4*nfacesfront);
		SplitFace(cubefront,&nfacesfront);
		cubetop.resize(4*nfacestop);
		SplitFace(cubetop,&nfacestop);
		cubebottom.resize(4*nfacesbottom);
		SplitFace(cubebottom,&nfacesbottom);	
		cubeleft.resize(4*nfacesleft);
		SplitFace(cubeleft,&nfacesleft);
		cuberight.resize(4*nfacesright);
		SplitFace(cuberight,&nfacesright);
		if(m_angle > 250){
			cubeback.resize(4*nfacesback);
			SplitFace(cubeback,&nfacesback);
		}
	}
	if(m_mode == DOME_OFFSET){ // double the resolution of the top (front)
			cubefront.resize(4*nfacesfront);
			SplitFace(cubefront,&nfacesfront);
			cubeback.resize(4*nfacesback);
			SplitFace(cubeback,&nfacesback);
	}
//*/

	// Turn into a hemisphere/sphere
	for(j=0;j<3;j++){
		for(i=0;i<nfacesfront;i++)
			cubefront[i].verts[j].normalize();
		for(i=0;i<nfacestop;i++)
			cubetop[i].verts[j].normalize();
		for(i=0;i<nfacesbottom;i++)
			cubebottom[i].verts[j].normalize();
		for(i=0;i<nfacesleft;i++)
			cubeleft[i].verts[j].normalize();
		for(i=0;i<nfacesright;i++)
			cuberight[i].verts[j].normalize();
		if(m_angle > 250)
				for(i=0;i<nfacesback;i++)
					cubeback[i].verts[j].normalize();
	}

/*
	// offseting the dome to work in a globe
	if(m_mode == DOME_OFFSET){
		for(j=0;j<3;j++){
			for(i=0;i<nfacesfront;i++){
				cubefront[i].verts[j][1] -= m_offset;
				cubefront[i].verts[j].normalize();
			}
			for(i=0;i<nfacesback;i++){
				cubeback[i].verts[j][1] -= m_offset;
				cubeback[i].verts[j].normalize();
			}
			for(i=0;i<nfacestop;i++){
				cubetop[i].verts[j][1] -= m_offset;
				cubetop[i].verts[j].normalize();
			}
			for(i=0;i<nfacesbottom;i++){
				cubebottom[i].verts[j][1] -= m_offset;
				cubebottom[i].verts[j].normalize();
			}
			for(i=0;i<nfacesleft;i++){
				cubeleft[i].verts[j][1] -= m_offset;
				cubeleft[i].verts[j].normalize();
			}
			for(i=0;i<nfacesright;i++){
				cuberight[i].verts[j][1] -= m_offset;
				cuberight[i].verts[j].normalize();
			}
		}
	}
//*/
	//flatten onto xz plane
	for(i=0;i<nfacesfront;i++)
		FlattenDome(cubefront[i].verts);	
	for(i=0;i<nfacestop;i++)
		FlattenDome(cubetop[i].verts);
	for(i=0;i<nfacesbottom;i++)
		FlattenDome(cubebottom[i].verts);
	for(i=0;i<nfacesleft;i++)
		FlattenDome(cubeleft[i].verts);		
	for(i=0;i<nfacesright;i++)
		FlattenDome(cuberight[i].verts);

	if(m_angle > 250)
		for(i=0;i<nfacesback;i++)
			FlattenDome(cubeback[i].verts);
}

void KX_Dome::CreateMeshPanorama(void)
{
/*
1)-  Define the faces of a cube without the top and bottom faces
 - each face is made out of 2 triangles
2) Subdivide the faces
 - more resolution == more curved lines
3) Spherize the cube
 - normalize the verts t
4) Flatten onto xz plane
 - use spherical projection techniques to transform the sphere onto a flat panorama
*/
	int i,j;
	float uv_ratio;
//	float verts_height;
//	float rad_ang = m_angle * MT_PI / 180.0;
	float sqrt_2 = sqrt(2.0);
//	verts_height = tan((rad_ang/2) - (MT_PI/2));//for 180 - M_PI/2 for 270 - 

	uv_ratio = (float)m_buffersize / m_imagesize;
	printf("uv_ratio: %4.2f\n", uv_ratio);//XXX

	/* Left Back (135�) face - two triangles */

	cubeleftback[0].verts[0][0] = 0;
	cubeleftback[0].verts[0][1] = -sqrt_2;
	cubeleftback[0].verts[0][2] = -1.0;
	cubeleftback[0].u[0] = 0;
	cubeleftback[0].v[0] = 0;

	cubeleftback[0].verts[1][0] = -sqrt_2;
	cubeleftback[0].verts[1][1] = 0;
	cubeleftback[0].verts[1][2] = -1.0;
	cubeleftback[0].u[1] = uv_ratio;
	cubeleftback[0].v[1] = 0;

	cubeleftback[0].verts[2][0] = 0;
	cubeleftback[0].verts[2][1] = -sqrt_2;
	cubeleftback[0].verts[2][2] = 1.0;
	cubeleftback[0].u[2] = 0;
	cubeleftback[0].v[2] = uv_ratio;

	cubeleftback[1].verts[0][0] = 0;
	cubeleftback[1].verts[0][1] = -sqrt_2;
	cubeleftback[1].verts[0][2] = 1.0;
	cubeleftback[1].u[0] = 0;
	cubeleftback[1].v[0] = uv_ratio;

	cubeleftback[1].verts[1][0] = -sqrt_2;
	cubeleftback[1].verts[1][1] = 0;
	cubeleftback[1].verts[1][2] = -1.0;
	cubeleftback[1].u[1] = uv_ratio;
	cubeleftback[1].v[1] = 0;

	cubeleftback[1].verts[2][0] = -sqrt_2;
	cubeleftback[1].verts[2][1] = 0;
	cubeleftback[1].verts[2][2] = 1.0;
	cubeleftback[1].u[2] = uv_ratio;
	cubeleftback[1].v[2] = uv_ratio;

	nfacesleftback = 2;

	/* Left face - two triangles */
	
	cubeleft[0].verts[0][0] = -sqrt_2;
	cubeleft[0].verts[0][1] = 0;
	cubeleft[0].verts[0][2] = -1.0;
	cubeleft[0].u[0] = 0;
	cubeleft[0].v[0] = 0;

	cubeleft[0].verts[1][0] = 0;
	cubeleft[0].verts[1][1] = sqrt_2;
	cubeleft[0].verts[1][2] = -1.0;
	cubeleft[0].u[1] = uv_ratio;
	cubeleft[0].v[1] = 0;

	cubeleft[0].verts[2][0] = -sqrt_2;
	cubeleft[0].verts[2][1] = 0;
	cubeleft[0].verts[2][2] = 1.0;
	cubeleft[0].u[2] = 0;
	cubeleft[0].v[2] = uv_ratio;

	cubeleft[1].verts[0][0] = -sqrt_2;
	cubeleft[1].verts[0][1] = 0;
	cubeleft[1].verts[0][2] = 1.0;
	cubeleft[1].u[0] = 0;
	cubeleft[1].v[0] = uv_ratio;

	cubeleft[1].verts[1][0] = 0;
	cubeleft[1].verts[1][1] = sqrt_2;
	cubeleft[1].verts[1][2] = -1.0;
	cubeleft[1].u[1] = uv_ratio;
	cubeleft[1].v[1] = 0;

	cubeleft[1].verts[2][0] = 0;
	cubeleft[1].verts[2][1] = sqrt_2;
	cubeleft[1].verts[2][2] = 1.0;
	cubeleft[1].u[2] = uv_ratio;
	cubeleft[1].v[2] = uv_ratio;

	nfacesleft = 2;
	
	/* Right face - two triangles */
	cuberight[0].verts[0][0] = 0;
	cuberight[0].verts[0][1] = sqrt_2;
	cuberight[0].verts[0][2] = -1.0;
	cuberight[0].u[0] = 0;
	cuberight[0].v[0] = 0;

	cuberight[0].verts[1][0] = sqrt_2;
	cuberight[0].verts[1][1] = 0;
	cuberight[0].verts[1][2] = -1.0;
	cuberight[0].u[1] = uv_ratio;
	cuberight[0].v[1] = 0;

	cuberight[0].verts[2][0] = sqrt_2;
	cuberight[0].verts[2][1] = 0;
	cuberight[0].verts[2][2] = 1.0;
	cuberight[0].u[2] = uv_ratio;
	cuberight[0].v[2] = uv_ratio;

	cuberight[1].verts[0][0] = 0;
	cuberight[1].verts[0][1] = sqrt_2;
	cuberight[1].verts[0][2] = -1.0;
	cuberight[1].u[0] = 0;
	cuberight[1].v[0] = 0;

	cuberight[1].verts[1][0] = sqrt_2;
	cuberight[1].verts[1][1] = 0;
	cuberight[1].verts[1][2] = 1.0;
	cuberight[1].u[1] = uv_ratio;
	cuberight[1].v[1] = uv_ratio;

	cuberight[1].verts[2][0] = 0;
	cuberight[1].verts[2][1] = sqrt_2;
	cuberight[1].verts[2][2] = 1.0;
	cuberight[1].u[2] = 0;
	cuberight[1].v[2] = uv_ratio;

	nfacesright = 2;
	
	/* Right Back  (-135�) face - two triangles */
	cuberightback[0].verts[0][0] = sqrt_2;
	cuberightback[0].verts[0][1] = 0;
	cuberightback[0].verts[0][2] = -1.0;
	cuberightback[0].u[0] = 0;
	cuberightback[0].v[0] = 0;

	cuberightback[0].verts[1][0] = 0;
	cuberightback[0].verts[1][1] = -sqrt_2;
	cuberightback[0].verts[1][2] = -1.0;
	cuberightback[0].u[1] = uv_ratio;
	cuberightback[0].v[1] = 0;

	cuberightback[0].verts[2][0] = 0;
	cuberightback[0].verts[2][1] = -sqrt_2;
	cuberightback[0].verts[2][2] = 1.0;
	cuberightback[0].u[2] = uv_ratio;
	cuberightback[0].v[2] = uv_ratio;

	cuberightback[1].verts[0][0] = sqrt_2;
	cuberightback[1].verts[0][1] = 0;
	cuberightback[1].verts[0][2] = -1.0;
	cuberightback[1].u[0] = 0;
	cuberightback[1].v[0] = 0;

	cuberightback[1].verts[1][0] = 0;
	cuberightback[1].verts[1][1] = -sqrt_2;
	cuberightback[1].verts[1][2] = 1.0;
	cuberightback[1].u[1] = uv_ratio;
	cuberightback[1].v[1] = uv_ratio;

	cuberightback[1].verts[2][0] = sqrt_2;
	cuberightback[1].verts[2][1] = 0;
	cuberightback[1].verts[2][2] = 1.0;
	cuberightback[1].u[2] = 0;
	cuberightback[1].v[2] = uv_ratio;

	nfacesrightback = 2;

	// Subdivide the faces
	for(i=0;i<m_resolution;i++){
		cubeleft.resize(4*nfacesleft);
		SplitFace(cubeleft,&nfacesleft);
		cuberight.resize(4*nfacesright);
		SplitFace(cuberight,&nfacesright);
		cubeleftback.resize(4*nfacesleftback);
		SplitFace(cubeleftback,&nfacesleftback);
		cuberightback.resize(4*nfacesrightback);
		SplitFace(cuberightback,&nfacesrightback);
	}

	// Spherize the cube
	for(j=0;j<3;j++){
		for(i=0;i<nfacesleftback;i++)
			cubeleftback[i].verts[j].normalize();
		for(i=0;i<nfacesleft;i++)
			cubeleft[i].verts[j].normalize();
		for(i=0;i<nfacesright;i++)
			cuberight[i].verts[j].normalize();
		for(i=0;i<nfacesrightback;i++)
			cuberightback[i].verts[j].normalize();
	}

	//Flatten onto xz plane
	for(i=0;i<nfacesleftback;i++)
		FlattenPanorama(cubeleftback[i].verts);
	for(i=0;i<nfacesleft;i++)
		FlattenPanorama(cubeleft[i].verts);
	for(i=0;i<nfacesright;i++)
		FlattenPanorama(cuberight[i].verts);
	for(i=0;i<nfacesrightback;i++)
		FlattenPanorama(cuberightback[i].verts);
}

void KX_Dome::FlattenDome(MT_Vector3 verts[3])
{
	double phi, r;
	float angle = m_angle * M_PI/180.0;

	for (int i=0;i<3;i++){
		r = atan2(sqrt(verts[i][0]*verts[i][0] + verts[i][2]*verts[i][2]), verts[i][1]);
		r /= angle/2;

		phi = atan2(verts[i][2], verts[i][0]);

		verts[i][0] = r * cos(phi);
		verts[i][1] = 0;
		verts[i][2] = r * sin(phi);

		if (r > 1.0){
		//round the border
			verts[i][0] = cos(phi);
			verts[i][1] = -3.0;
			verts[i][2] = sin(phi);
		}
	}
}

void KX_Dome::FlattenPanorama(MT_Vector3 verts[3])
{
	//: increase, remove backward faces
	double phi;
	float angle = m_angle * M_PI/180.0;

	for (int i=0;i<3;i++){
		phi = atan2(verts[i][1], verts[i][0]);
		phi *= -1.0; //flipping
		
		verts[i][0] = phi/ MT_PI;
		verts[i][1] = 0;

		if(verts[i][2] > 0.5)
			verts[i][2] = 0.5;
		else if(verts[i][2] < -0.5)
			verts[i][2] = -0.5;

		verts[i][2] = atan2(verts[i][2], 1.0)/ MT_PI * 2;

/*
		if(verts[i][2] > 0.5)
			verts[i][2] = 0.5;
		else if(verts[i][2] < -0.5)
			verts[i][2] = -0.5;
//*/
	}

}

void KX_Dome::SplitFace(vector <DomeFace>& face, int *nfaces)
{
	int i;
	int n1, n2;

	n1 = n2 = *nfaces;

	for(i=0;i<n1;i++){

		face[n2].verts[0] = (face[i].verts[0] + face[i].verts[1]) /2;
		face[n2].verts[1] =  face[i].verts[1];
		face[n2].verts[2] = (face[i].verts[1] + face[i].verts[2]) /2;
		face[n2].u[0]	  = (face[i].u[0] + face[i].u[1]) /2;
		face[n2].u[1]	  =  face[i].u[1];
		face[n2].u[2]	  = (face[i].u[1] + face[i].u[2]) /2;
		face[n2].v[0]	  = (face[i].v[0] + face[i].v[1]) /2;
		face[n2].v[1]	  =  face[i].v[1];
		face[n2].v[2]	  = (face[i].v[1] + face[i].v[2]) /2;

		face[n2+1].verts[0] = (face[i].verts[1] + face[i].verts[2]) /2;
		face[n2+1].verts[1] =  face[i].verts[2];
		face[n2+1].verts[2] = (face[i].verts[2] + face[i].verts[0]) /2;
		face[n2+1].u[0]		= (face[i].u[1] + face[i].u[2]) /2;
		face[n2+1].u[1]		=  face[i].u[2];
		face[n2+1].u[2]		= (face[i].u[2] + face[i].u[0]) /2;
		face[n2+1].v[0]		= (face[i].v[1] + face[i].v[2]) /2;
		face[n2+1].v[1]		=  face[i].v[2];
		face[n2+1].v[2]		= (face[i].v[2] + face[i].v[0]) /2;

		face[n2+2].verts[0] = (face[i].verts[0] + face[i].verts[1]) /2;
		face[n2+2].verts[1] = (face[i].verts[1] + face[i].verts[2]) /2;
		face[n2+2].verts[2] = (face[i].verts[2] + face[i].verts[0]) /2;
		face[n2+2].u[0]	  = (face[i].u[0] + face[i].u[1]) /2;
		face[n2+2].u[1]	  = (face[i].u[1] + face[i].u[2]) /2;
		face[n2+2].u[2]	  = (face[i].u[2] + face[i].u[0]) /2;
		face[n2+2].v[0]	  = (face[i].v[0] + face[i].v[1]) /2;
		face[n2+2].v[1]	  = (face[i].v[1] + face[i].v[2]) /2;
		face[n2+2].v[2]	  = (face[i].v[2] + face[i].v[0]) /2;		

		//face[i].verts[0] = face[i].verts[0] ;
		face[i].verts[1] = (face[i].verts[0] + face[i].verts[1]) /2;
		face[i].verts[2] = (face[i].verts[0] + face[i].verts[2]) /2;
		//face[i].u[0]	 =  face[i].u[0];
		face[i].u[1]	 = (face[i].u[0] + face[i].u[1]) /2;
		face[i].u[2]	 = (face[i].u[0] + face[i].u[2]) /2;
		//face[i].v[0]	 = face[i].v[0] ;
		face[i].v[1]	 = (face[i].v[0] + face[i].v[1]) /2;
		face[i].v[2]	 = (face[i].v[0] + face[i].v[2]) /2;		

		n2 += 3; // number of faces
	}
	*nfaces = n2;
}

void KX_Dome::CalculateFrustum(KX_Camera * m_camera)
{
	//  manually creating a 90� Field of View Frustum 

	/*
	 the original formula:
	top = tan(fov*3.14159/360.0) * near [for fov in degrees]
	fov*0.5 = arctan ((top-bottom)*0.5 / near) [for fov in radians]
	bottom = -top
	left = aspect * bottom
	right = aspect * top
	*/

	RAS_FrameFrustum m_frustrum; //90 deg. Frustum

	m_frustrum.camnear = m_camera->GetCameraNear();
	m_frustrum.camfar = m_camera->GetCameraFar();

//	float top = tan(90.0*MT_PI/360.0) * m_frustrum.camnear;
	float top = m_frustrum.camnear; // for deg = 90�, tan = 1

	m_frustrum.x1 = -top;
	m_frustrum.x2 = top;
	m_frustrum.y1 = -top;
	m_frustrum.y2 = top;
	
	m_projmat = m_rasterizer->GetFrustumMatrix(
	m_frustrum.x1, m_frustrum.x2, m_frustrum.y1, m_frustrum.y2, m_frustrum.camnear, m_frustrum.camfar);

	m_camera->SetProjectionMatrix(m_projmat); //this can be moved somewhere else to be runned while rendering

	/*
//	the glu call:
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(90.0,1.0,m_camera->GetCameraNear(),m_camera->GetCameraFar());
	/**/
}

void KX_Dome::CalculateCameraOrientation()
{
/*
Uses 4 cameras for angles up to 180�
Uses 5 camera for angles up to 250 �
*/
	float deg45 = MT_PI / 4;
	float deg135 = 3 * deg45;
	MT_Scalar c = cos(deg45);
	MT_Scalar s = sin(deg45);

	if ((m_mode == DOME_FISHEYE && m_angle <= 180)|| m_mode == DOME_TRUNCATED || m_mode == DOME_WARPED){

		m_locRot[0] = MT_Matrix3x3( // 90� - Top
						c, -s, 0.0,
						0.0,0.0, -1.0,
						s, c, 0.0);

		m_locRot[1] = MT_Matrix3x3( // 90� - Bottom
						-s, c, 0.0,
						0.0,0.0, 1.0,
						s, c, 0.0);

		m_locRot[2] = MT_Matrix3x3( // 45� - Left
						c, 0.0, s,
						0, 1.0, 0.0,
						-s, 0.0, c);

		m_locRot[3] = MT_Matrix3x3( // 45� - Right
						c, 0.0, -s,
						0.0, 1.0, 0.0,
						s, 0.0, c);

	}else if ((m_mode == DOME_FISHEYE && m_angle > 180)||m_mode == DOME_OFFSET){
	//WIP
		m_locRot[0] = MT_Matrix3x3( // 90� - Top
						 1.0, 0.0, 0.0,
						 0.0, 0.0,-1.0,
						 0.0, 1.0, 0.0);

		m_locRot[1] = MT_Matrix3x3( // 90� - Bottom
						 1.0, 0.0, 0.0,
						 0.0, 0.0, 1.0,
						 0.0,-1.0, 0.0);

		m_locRot[2] = MT_Matrix3x3( // -90� - Left
						 0.0, 0.0, 1.0,
						 0.0, 1.0, 0.0,
						 -1.0, 0.0, 0.0);

		m_locRot[3] = MT_Matrix3x3( // 90� - Right
						 0.0, 0.0,-1.0,
						 0.0, 1.0, 0.0,
						 1.0, 0.0, 0.0);
						
		m_locRot[4] = MT_Matrix3x3( // 0� - Front
						1.0, 0.0, 0.0,
						0.0, 1.0, 0.0,
						0.0, 0.0, 1.0);

		m_locRot[5] = MT_Matrix3x3( // 180� - Back
						-1.0, 0.0, 0.0,
						 0.0, 1.0, 0.0,
						 0.0, 0.0,-1.0);

	} else if (m_mode == DOME_PANORAM_SPH){

		m_locRot[0] = MT_Matrix3x3( // 135� - Left
						-s, 0.0, c,
						0, 1.0, 0.0,
						-c, 0.0, -s);

		m_locRot[1] = MT_Matrix3x3( // 45� - Left
						c, 0.0, s,
						0, 1.0, 0.0,
						-s, 0.0, c);

		m_locRot[2] = MT_Matrix3x3( // 45� - Right
						c, 0.0, -s,
						0.0, 1.0, 0.0,
						s, 0.0, c);

		m_locRot[3] = MT_Matrix3x3( // 135� - Right
						-s, 0.0, -c,
						0.0, 1.0, 0.0,
						c, 0.0, -s);
	} else {
		m_locRot[0] = MT_Matrix3x3( // 90� - Top
						0.0,-1.0, 0.0,
						0.0, 0.0,-1.0,
						1.0, 0.0, 0.0);

		m_locRot[1] = MT_Matrix3x3( // 90� - Bottom
						-1.0, 0.0, 0.0,
						 0.0, 0.0, 1.0,
						 0.0, 1.0, 0.0);

		m_locRot[2] = MT_Matrix3x3( // 90� - Left
						 0.0, 0.0, 1.0,
						 0.0, 1.0, 0.0,
						-1.0, 0.0, 0.0);

		m_locRot[3] = MT_Matrix3x3( // 90� - Right
						0.0, 0.0,-1.0,
						0.0, 1.0, 0.0,
						1.0, 0.0, 0.0);

		m_locRot[4] = MT_Matrix3x3( // 0� - Front
						1.0, 0.0, 0.0,
						0.0, 1.0, 0.0,
						0.0, 0.0, 1.0);

		m_locRot[5] = MT_Matrix3x3( // 180� - Back
						-1.0, 0.0, 0.0,
						 0.0, 1.0, 0.0,
						 1.0, 0.0,-1.0);
	}
}

void KX_Dome::RotateCamera(KX_Camera* m_camera, int i)
{
// I'm not using it, I'm doing inline call for these commands
// but it's nice to have it here in case I need it

	MT_Matrix3x3 camori = m_camera->GetSGNode()->GetLocalOrientation();

	m_camera->NodeSetLocalOrientation(camori*m_locRot[i]);
	m_camera->NodeUpdateGS(0.f,true);

	MT_Transform camtrans(m_camera->GetWorldToCamera());
	MT_Matrix4x4 viewmat(camtrans);
	m_rasterizer->SetViewMatrix(viewmat, m_camera->NodeGetWorldPosition(),
		m_camera->GetCameraLocation(), m_camera->GetCameraOrientation());
	m_camera->SetModelviewMatrix(viewmat);

	// restore the original orientation
	m_camera->NodeSetLocalOrientation(camori);
	m_camera->NodeUpdateGS(0.f,true);
}

void KX_Dome::Draw(void)
{
	/* XXX
	I think the best solution here is to calculate the framing and other stuffs beforehand,
	*/
	switch(m_mode){
		case DOME_FISHEYE:
			DrawDomeFisheye();
			break;
		case DOME_TRUNCATED:
			DrawDomeFisheye();
			break;
		case DOME_PANORAM_SPH:
			DrawPanorama();
			break;
		case DOME_OFFSET:
			DrawDomeFisheye();
			break;
		case DOME_WARPED: //tmp
			DrawDomeTmp();
			break;
		default:
			DrawDomeTmp();
			break;
	}
}

void KX_Dome::DrawDomeFisheye(void)
{
	//correct, dome projection from 4 renders
	int i,j;

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();

	// Making the viewport always square 

	int can_width = m_viewport.GetRight();
	int can_height = m_viewport.GetTop();

	float ortho_width, ortho_height;

	if (m_mode == DOME_TRUNCATED){
			ortho_width = 1.0;
			ortho_height = 2 * ((float)can_height/can_width) - 1.0 ;
			
			ortho_width /= m_size;
			ortho_height /= m_size;

			glOrtho((-ortho_width), ortho_width, (-ortho_height), ortho_width, -20.0, 10.0);
			
	} else {
		if (can_width < can_height){
			ortho_width = 1.0;
			ortho_height = (float)can_height/can_width;
		}else{
			ortho_width = (float)can_width/can_height;
			ortho_height = 1.0;
		}
		
		ortho_width /= m_size;
		ortho_height /= m_size;
		
		glOrtho((-ortho_width), ortho_width, (-ortho_height), ortho_height, -20.0, 10.0);
	}

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	gluLookAt(0.0,-1.0,0.0, 0.0,0.0,0.0, 0.0,0.0,1.0);

//	glDisable(GL_DEPTH_TEST);
	glEnable(GL_DEPTH_TEST);
	glPolygonMode(GL_FRONT, GL_FILL);
	glPolygonMode(GL_BACK, GL_LINE);	
	glShadeModel(GL_SMOOTH);
	glDisable(GL_LIGHTING);

	glEnable(GL_TEXTURE_2D);
	glColor3f(1.0,1.0,1.0);

	if (dlistSupported){
		for(i=0;i<m_numfaces;i++){
			glBindTexture(GL_TEXTURE_2D, domefacesId[i]);
			glCallList(dlistId+i);
		}
	}
	else { // DisplayLists not supported
		// top triangle
		glBindTexture(GL_TEXTURE_2D, domefacesId[0]);
		GLDrawTriangles(cubetop, nfacestop);

		// bottom triangle	
		glBindTexture(GL_TEXTURE_2D, domefacesId[1]);
		GLDrawTriangles(cubebottom, nfacesbottom);

		// left triangle
		glBindTexture(GL_TEXTURE_2D, domefacesId[2]);
		GLDrawTriangles(cubeleft, nfacesleft);

		// right triangle
		glBindTexture(GL_TEXTURE_2D, domefacesId[3]);
		GLDrawTriangles(cuberight, nfacesright);

		if (m_angle > 180){
			// front triangle
			glBindTexture(GL_TEXTURE_2D, domefacesId[4]);
			GLDrawTriangles(cubefront, nfacesfront);
		}
		if (m_angle > 250){
			// backtriangle
			glBindTexture(GL_TEXTURE_2D, domefacesId[5]);
			GLDrawTriangles(cubeback, nfacesback);
		}
	}
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_DEPTH_TEST);
}

void KX_Dome::DrawPanorama(void)
{
	int i,j;
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();

	// Making the viewport always square 

	int can_width = m_viewport.GetRight();
	int can_height = m_viewport.GetTop();

	float ortho_height = 1.0;
	float ortho_width = 1.0;

	//using all the screen
	if ((can_width / 4) <= (can_height)){
		ortho_width = 1.0;
		ortho_height = (float)can_height/can_width;
	}else{
		ortho_width = (float)can_width/can_height * 0.75;
		ortho_height = 0.75;
	}

	ortho_width /= m_size;
	ortho_height /= m_size;
	
	glOrtho((-ortho_width), ortho_width, (-ortho_height), ortho_height, -20.0, 10.0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	gluLookAt(0.0,-1.0,0.0, 0.0,0.0,0.0, 0.0,0.0,1.0);

	glDisable(GL_DEPTH_TEST);

	//I'm culling because there are some faces backwarded
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glPolygonMode(GL_BACK, GL_LINE);
	glPolygonMode(GL_FRONT, GL_FILL);
	glShadeModel(GL_SMOOTH);
	glDisable(GL_LIGHTING);

	glEnable(GL_TEXTURE_2D);
	glColor3f(1.0,1.0,1.0);

//	m_numfaces = 2;
	if (dlistSupported){
		for(i=0;i<m_numfaces;i++){
			glBindTexture(GL_TEXTURE_2D, domefacesId[i]);
			glCallList(dlistId+i);
		}
	}
	else {
		// domefacesId[1] => -45� (left)
		glBindTexture(GL_TEXTURE_2D, domefacesId[1]);
			GLDrawTriangles(cubeleft, nfacesleft);

		// domefacesId[2] => 45� (right)
		glBindTexture(GL_TEXTURE_2D, domefacesId[2]);
			GLDrawTriangles(cuberight, nfacesright);

		// domefacesId[3] => 135� (rightback)
		glBindTexture(GL_TEXTURE_2D, domefacesId[3]);
			GLDrawTriangles(cuberightback, nfacesrightback);

		// domefacesId[0] => -135� (left)
		glBindTexture(GL_TEXTURE_2D, domefacesId[0]);
			GLDrawTriangles(cubeleftback, nfacesleftback);
	}
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_DEPTH_TEST);
}

void KX_Dome::BindImages(int i)
{
	RAS_Rect canvas_rect = m_canvas->GetWindowArea();
	int left = canvas_rect.GetLeft();
	int bottom = canvas_rect.GetBottom();

	left += m_imagesize;
	bottom  += m_imagesize;

	glBindTexture(GL_TEXTURE_2D, domefacesId[i]);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_viewport.GetLeft(), m_viewport.GetBottom(), m_imagesize, m_imagesize, 0); //XXX WORKS

//	printf("\nm_viewport.GetBottom(): %d\n", m_viewport.GetBottom());
//	printf("m_viewport.GetLeft(): %d\n", m_viewport.GetLeft());
//	printf("canvas_rect.GetBottom(): %d\n", bottom);
//	printf("canvas_rect.GetLeft(): %d\n", left);
}

void KX_Dome::SetViewPort(GLuint viewport[4])
{
	if(canvaswidth != m_canvas->GetWidth() || canvasheight != m_canvas->GetHeight())
	{
		m_viewport.SetLeft(viewport[0]); 
		m_viewport.SetBottom(viewport[1]);
		m_viewport.SetRight(viewport[2]);
		m_viewport.SetTop(viewport[3]);

		CalculateImageSize();
//		ClearGLImages();
//		CreateGLImages();
	}
}

void KX_Dome::RenderDomeFrame(KX_Scene* scene, KX_Camera* cam, int i)
{
	bool override_camera;
//	RAS_Rect viewport, area;
	float left, right, bottom, top, nearfrust, farfrust, focallength;
	const float ortho = 100.0;
	float m_cameraZoom = 1.0; //tmp
//	KX_Camera* cam = scene->GetActiveCamera();
	
	if (!cam)
		return;

//	m_canvas->SetViewPort(0,0,m_imagesize-1,m_imagesize-1);
	m_canvas->SetViewPort(0,0,m_buffersize,m_buffersize);
//	m_canvas->SetViewPort(m_viewport.GetLeft(), m_viewport.GetBottom(), m_buffersize,m_buffersize);
	// see KX_BlenderMaterial::Activate
	//m_rasterizer->SetAmbient();
	m_rasterizer->DisplayFog();
	
	CalculateFrustum(cam);
//	Dome_RotateCamera(cam,i);

	MT_Matrix3x3 camori = cam->GetSGNode()->GetLocalOrientation();

	cam->NodeSetLocalOrientation(camori*m_locRot[i]);
	cam->NodeUpdateGS(0.f,true);

	MT_Transform camtrans(cam->GetWorldToCamera());
	MT_Matrix4x4 viewmat(camtrans);
	m_rasterizer->SetViewMatrix(viewmat, cam->NodeGetWorldPosition(),
		cam->GetCameraLocation(), cam->GetCameraOrientation());
	cam->SetModelviewMatrix(viewmat);

//	scene->UpdateMeshTransformations();//I need to run it somewherelse, otherwise Im overrunning it

	// The following actually reschedules all vertices to be
	// redrawn. There is a cache between the actual rescheduling
	// and this call though. Visibility is imparted when this call
	// runs through the individual objects.

//	MT_Transform camtrans(cam->GetWorldToCamera());

	scene->CalculateVisibleMeshes(m_rasterizer,cam);

	scene->RenderBuckets(camtrans, m_rasterizer, m_rendertools);
	
		// restore the original orientation
	cam->NodeSetLocalOrientation(camori);
	cam->NodeUpdateGS(0.f,true);
}
/*
void KX_Dome::clearBuffer(int i){
	m_canvas->ClearBuffer(RAS_ICanvas::COLOR_BUFFER|RAS_ICanvas::DEPTH_BUFFER);
}
*/

void KX_Dome::DrawDomeTmp(void)
{
	//Skybox => 4 images
//	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();

	// Making the viewport always square 

	int can_width = m_viewport.GetRight();
	int can_height = m_viewport.GetTop();

	float ortho_width, ortho_height = 1.0;

	//using all the screen
	if ((can_width / 4) <= (can_height / 3)){
		ortho_width = 1.0;
		ortho_height = (float)can_height/can_width;
	}else{
		ortho_width = (float)can_width/can_height * 0.75;
		ortho_height = 0.75;
	}
	glOrtho((-ortho_width), ortho_width, (-ortho_height), ortho_height, -20.0, 10.0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glPolygonMode(GL_FRONT, GL_FILL);
	glShadeModel(GL_SMOOTH);
	glDisable(GL_LIGHTING);

	glEnable(GL_TEXTURE_2D);
	glColor3f(1.0f ,1.0f, 1.0f);

// domefacesId[0]-> TOP
	glBindTexture(GL_TEXTURE_2D, domefacesId[0]);
	glBegin(GL_QUADS);
		glTexCoord2f(1.0,1.0);
		glVertex3f( 0.0f, 0.75f, 3.0f);
		glTexCoord2f(0.0,1.0);
		glVertex3f(-0.5f,0.75f, 3.0f);
		glTexCoord2f(0.0,0.0);
		glVertex3f(-0.5f,0.25f, 3.0f);
		glTexCoord2f(1.0,0.0);
		glVertex3f(0.0f,0.25f, 3.0f);
	glEnd();

// domefacesId[1]-> BOTTOM
	glBindTexture(GL_TEXTURE_2D, domefacesId[1]);
	glBegin(GL_QUADS);
		glTexCoord2f(1.0,1.0);
		glVertex3f( 0.0f,-0.25f, 3.0f);
		glTexCoord2f(0.0,1.0);
		glVertex3f(-0.5f,-0.25f, 3.0f);
		glTexCoord2f(0.0,0.0);
		glVertex3f(-0.5f,-0.75f, 3.0f);
		glTexCoord2f(1.0,0.0);
		glVertex3f(0.0f,-0.75f, 3.0f);
	glEnd();

// domefacesId[2]-> LEFT
	glBindTexture(GL_TEXTURE_2D, domefacesId[2]);
	glBegin(GL_QUADS);
		glTexCoord2f(1.0,1.0);
		glVertex3f( -0.5f, 0.25f, 3.0f);
		glTexCoord2f(0.0,1.0);
		glVertex3f(-1.0f,0.25f, 3.0f);
		glTexCoord2f(0.0,0.0);
		glVertex3f(-1.0f,-0.25f, 3.0f);
		glTexCoord2f(1.0,0.0);
		glVertex3f(-0.5f,-0.25f, 3.0f);
	glEnd();

// domefacesId[3]-> RIGHT
	glBindTexture(GL_TEXTURE_2D, domefacesId[3]);
	glBegin(GL_QUADS);
		glTexCoord2f(1.0,1.0);
		glVertex3f( 0.5f, 0.25f, 3.0f);
		glTexCoord2f(0.0,1.0);
		glVertex3f(0.0f,0.25f, 3.0f);
		glTexCoord2f(0.0,0.0);
		glVertex3f(0.0f,-0.25f, 3.0f);
		glTexCoord2f(1.0,0.0);
		glVertex3f( 0.5f,-0.25f, 3.0f);
	glEnd();

// domefacesId[4]-> FRONT
	glBindTexture(GL_TEXTURE_2D, domefacesId[4]);
	glBegin(GL_QUADS);
		glTexCoord2f(1.0,1.0);
		glVertex3f( 0.0f, 0.25f, 3.0f);
		glTexCoord2f(0.0,1.0);
		glVertex3f(-0.5f,0.25f, 3.0f);
		glTexCoord2f(0.0,0.0);
		glVertex3f(-0.5f,-0.25f, 3.0f);
		glTexCoord2f(1.0,0.0);
		glVertex3f(0.0f,-0.25f, 3.0f);
	glEnd();

// domefacesId[5]-> BACK
	glBindTexture(GL_TEXTURE_2D, domefacesId[5]);
	glBegin(GL_QUADS);
		glTexCoord2f(1.0,1.0);
		glVertex3f( 1.0f, 0.25f, 3.0f);
		glTexCoord2f(0.0,1.0);
		glVertex3f(0.5f,0.25f, 3.0f);
		glTexCoord2f(0.0,0.0);
		glVertex3f(0.5f,-0.25f, 3.0f);
		glTexCoord2f(1.0,0.0);
		glVertex3f(1.0f,-0.25f, 3.0f);
	glEnd();

	glDisable(GL_TEXTURE_2D);
	glEnable(GL_DEPTH_TEST);
}